#include "world.hpp"

#ifdef FVMAP_WITH_MUTSEA
#include <mutsea/projection.h>
#include <mutsea/replica.h>
#endif
#include <cmath>
#include <cstring>

namespace fvmap {

struct WorldService::World {
  const WorldConfig *config = nullptr;
  std::mutex m;
#ifdef FVMAP_WITH_MUTSEA
  ms_projection *proj = nullptr;
  // Ω-XVII.3: a live world is a read-only subscriber of the authority's replication stream.
  ms_replica *live = nullptr;
  std::string live_world, live_region, host, token, checkpoint;
  ~World() {
    if (proj) ms_projection_close(proj);
    if (live) ms_replica_stop(live);
  }
#endif
};

#ifdef FVMAP_WITH_MUTSEA
namespace {

[[noreturn]] void fail(ms_projection *p, ms_projection_status s) {
  std::string msg = p ? ms_projection_last_error(p) : "projection unavailable";
  switch (s) {
    case MS_PROJ_GAP: throw Error(409, "resync_required", msg);
    case MS_PROJ_ARGUMENT: throw Error(400, "invalid_parameter", msg);
    case MS_PROJ_BUSY: throw Error(503, "world_busy", msg);
    case MS_PROJ_CORRUPT: throw Error(500, "world_integrity", msg);
    default: throw Error(503, "world_unavailable", msg);
  }
}

json::value take_json(ms_projection *p, ms_projection_status s, char *text, std::size_t len) {
  if (s != MS_PROJ_OK) fail(p, s);
  std::string copy(text, len);
  ms_projection_free(text);
  boost::system::error_code ec;
  json::value v = json::parse(copy, ec);
  if (ec) throw Error(500, "world_integrity", "projection produced invalid JSON");
  return v;
}


std::string hex_bytes(const uint8_t *p, std::size_t n) {
  static const char *d = "0123456789abcdef";
  std::string s;
  for (std::size_t i = 0; i < n; ++i) s += d[p[i] >> 4], s += d[p[i] & 15];
  return s;
}

bool uuid_bytes(const std::string &text, uint8_t out[16]) {
  std::string h;
  for (char c : text)
    if (c != '-') h += c;
  if (h.size() != 32 || h.find_first_not_of("0123456789abcdef") != std::string::npos) return false;
  for (int i = 0; i < 16; ++i) out[i] = static_cast<uint8_t>(std::stoul(h.substr(static_cast<std::size_t>(2 * i), 2), nullptr, 16));
  return true;
}

const char *state_name(ms_replica_state s) {
  switch (s) {
    case MS_REPLICA_LIVE: return "live";
    case MS_REPLICA_SYNCING: return "syncing";
    case MS_REPLICA_CONNECTING: return "connecting";
    case MS_REPLICA_BACKOFF: return "reconnecting";
    case MS_REPLICA_REFUSED: return "refused";
    case MS_REPLICA_STOPPED: return "stopped";
    default: return "idle";
  }
}

ms_replica_status live_status(ms_replica *r) {
  ms_replica_status st{};
  st.struct_size = sizeof st;
  ms_replica_get_status(r, &st);
  return st;
}

json::value parse_owned(char *text, std::size_t len) {
  std::string copy(text, len);
  ms_replica_free(text);
  boost::system::error_code ec;
  json::value v = json::parse(copy, ec);
  if (ec) throw Error(500, "world_integrity", "live projection produced invalid JSON");
  return v;
}
}  // namespace
#endif

#ifdef FVMAP_WITH_MUTSEA
static void start_live(WorldService::World &w, const Config &config);
#endif

WorldService::WorldService(const Config &config) : config_(config) {
  for (const auto &w : config.worlds) {
    auto world = std::make_unique<World>();
    world->config = &w;
#ifdef FVMAP_WITH_MUTSEA
    if (w.live_tokens) {
      start_live(*world, config);
    } else {
      ms_projection_status s = ms_projection_open(w.store.c_str(), &world->proj);
      if (s != MS_PROJ_OK)
        throw std::runtime_error("world " + w.id + ": " +
                                 (world->proj ? ms_projection_last_error(world->proj) : std::string("cannot open store")));
    }
#endif
    worlds_.push_back(std::move(world));
  }
}


#ifdef FVMAP_WITH_MUTSEA
static void start_live(WorldService::World &w, const Config &config) {
  std::string bytes = read_file(*w.config->live_tokens, 1 << 16);
  boost::system::error_code ec;
  json::value doc = json::parse(bytes, ec);
  if (ec || !doc.is_object()) throw std::runtime_error("world " + w.config->id + ": live token file is not JSON");
  const auto &o = doc.as_object();
  w.live_world = std::string(o.at("world").as_string());
  w.live_region = std::string(o.at("region").as_string());
  std::string repl(o.at("replication").as_string());
  w.token = std::string(o.at("tokens").as_object().at("observer").as_string());
  auto colon = repl.rfind(':');
  if (colon == std::string::npos) throw std::runtime_error("live replication address");
  w.host = repl.substr(0, colon);
  w.checkpoint = (config.state_dir / ("world-" + w.config->id + ".replica")).string();
  ms_replica_config c{};
  c.struct_size = sizeof c;
  c.host = w.host.c_str();
  c.port = static_cast<uint16_t>(std::stoul(repl.substr(colon + 1)));
  c.token = w.token.c_str();
  c.checkpoint_path = w.checkpoint.c_str();
  if (!uuid_bytes(w.live_world, c.world) || !uuid_bytes(w.live_region, c.region)) throw std::runtime_error("live world/region ids");
  if (ms_replica_start(&c, &w.live) != MS_REPLICA_OK) throw std::runtime_error("world " + w.config->id + ": live replica did not start");
}

// Live head: every response names the world, generation and revision it describes,
// and a projection that is not currently synchronized says so explicitly.
static json::object live_head(WorldService::World &w) {
  auto st = live_status(w.live);
  const bool live = st.state == MS_REPLICA_LIVE;
  return json::object{{"source", "mutsea_live_replication"}, {"protocol", "finalverse.world-replication/1"},
                      {"world", w.live_world}, {"region", w.live_region}, {"generation", hex_bytes(st.generation, 32)},
                      {"state", state_name(st.state)}, {"mode", st.mode}, {"current", live},
                      {"revision", st.revision}, {"head_revision", st.head_revision}, {"lagging", !live || st.revision < st.head_revision},
                      {"worldline", json::object{{"length", st.worldline_length}, {"hash", hex_bytes(st.worldline_hash, 32)}, {"verified", true}}},
                      {"entity_count", st.entity_count}, {"commits_applied", st.commits_applied}, {"reconnects", st.reconnects},
                      {"resyncs", st.resyncs}, {"latency_ms", json::object{{"p50", st.p50_latency_ms}, {"p95", st.p95_latency_ms}}},
                      {"open_mode", "read_only_subscriber"}};
}

static void require_current(WorldService::World &w) {
  auto st = live_status(w.live);
  if (st.state != MS_REPLICA_LIVE || !(st.snapshots || st.commits_applied))
    throw Error(503, "world_not_current",
                std::string("live projection is ") + state_name(st.state) + " at revision " + std::to_string(st.revision) +
                    "; stale state is never served as current");
}
#endif

WorldService::~WorldService() = default;

bool WorldService::enabled() const {
#ifdef FVMAP_WITH_MUTSEA
  return true;
#else
  return false;
#endif
}

const WorldConfig *WorldService::find(const std::string &id) const {
  for (auto &w : worlds_)
    if (w->config->id == id) return w->config;
  return nullptr;
}

WorldService::World &WorldService::get(const std::string &id) {
  if (!enabled()) throw Error(501, "world_api_disabled", "this build has no MutSea projection");
  for (auto &w : worlds_)
    if (w->config->id == id) return *w;
  throw Error(404, "unknown_world", "world is not configured");
}

json::array WorldService::list() {
  json::array out;
  for (auto &w : worlds_) {
    json::object o{{"id", w->config->id}, {"title", w->config->title},
                   {"dataset", w->config->dataset.empty() ? json::value(nullptr) : json::value(w->config->dataset)},
                   {"authority", "mutsea"}, {"access", "read_only_projection"}};
    if (enabled()) {
      try {
        o["head"] = head(w->config->id);
      } catch (const Error &e) {
        o["head"] = json::object{{"error", e.code}};
      }
    }
    out.push_back(o);
  }
  return out;
}

json::object WorldService::head(const std::string &id) {
  World &w = get(id);
#ifdef FVMAP_WITH_MUTSEA
  std::lock_guard lock(w.m);
  if (w.live) return live_head(w);
  int changed = 0;
  if (auto s = ms_projection_refresh(w.proj, &changed); s != MS_PROJ_OK) fail(w.proj, s);
  char *text = nullptr;
  std::size_t len = 0;
  auto s = ms_projection_head_json(w.proj, &text, &len);
  return take_json(w.proj, s, text, len).as_object();
#else
  (void)w;
  return {};
#endif
}

json::object WorldService::snapshot(const std::string &id) {
  World &w = get(id);
#ifdef FVMAP_WITH_MUTSEA
  std::lock_guard lock(w.m);
  if (w.live) {
    require_current(w);
    auto head = live_head(w);
    char *text = nullptr;
    std::size_t len = 0;
    if (ms_replica_entities_json(w.live, &text, &len) != MS_REPLICA_OK) throw Error(503, "world_unavailable", "live entities");
    head["entities"] = parse_owned(text, len);
    // Re-check: the snapshot must describe the revision it claims.
    if (live_status(w.live).revision != head["revision"].as_uint64()) throw Error(503, "world_moving", "revision advanced during the snapshot; retry");
    return head;
  }
  int changed = 0;
  if (auto s = ms_projection_refresh(w.proj, &changed); s != MS_PROJ_OK) fail(w.proj, s);
  char *text = nullptr;
  std::size_t len = 0;
  auto s = ms_projection_snapshot_json(w.proj, &text, &len);
  return take_json(w.proj, s, text, len).as_object();
#else
  (void)w;
  return {};
#endif
}

json::object WorldService::events(const std::string &id, uint64_t after, const std::string &after_hash, uint32_t limit) {
  World &w = get(id);
#ifdef FVMAP_WITH_MUTSEA
  std::lock_guard lock(w.m);
  if (w.live) {
    require_current(w);
    char *text = nullptr;
    std::size_t len = 0;
    if (ms_replica_recent_events_json(w.live, &text, &len) != MS_REPLICA_OK) throw Error(503, "world_unavailable", "live events");
    auto recent = parse_owned(text, len).as_array();
    auto st = live_status(w.live);
    uint64_t first = recent.empty() ? st.worldline_length + 1 : recent.front().as_object().at("seq").to_number<uint64_t>();
    if (after > st.worldline_length || (after + 1 < first && after < st.worldline_length))
      throw Error(409, "resync_required", "cursor outside the live window; take a snapshot or use the replication stream");
    json::array out;
    for (auto &e : recent) {
      auto seq = e.as_object().at("seq").to_number<uint64_t>();
      if (seq == after && !after_hash.empty() && std::string(e.as_object().at("hash").as_string()) != after_hash)
        throw Error(409, "resync_required", "cursor hash is not on this chain");
      if (seq > after && out.size() < limit) out.push_back(e);
    }
    return json::object{{"world", w.live_world}, {"generation", hex_bytes(st.generation, 32)}, {"events", out},
                        {"head", st.worldline_length}, {"complete", out.empty() || out.back().as_object().at("seq").to_number<uint64_t>() == st.worldline_length}};
  }
  int changed = 0;
  if (auto s = ms_projection_refresh(w.proj, &changed); s != MS_PROJ_OK) fail(w.proj, s);
  char *text = nullptr;
  std::size_t len = 0;
  auto s = ms_projection_events_json(w.proj, after, after_hash.empty() ? nullptr : after_hash.c_str(), limit, &text, &len);
  return take_json(w.proj, s, text, len).as_object();
#else
  (void)w, (void)after, (void)after_hash, (void)limit;
  return {};
#endif
}

std::string WorldService::events_fvp(const std::string &id, uint64_t after, const std::string &after_hash, uint32_t limit) {
  World &w = get(id);
#ifdef FVMAP_WITH_MUTSEA
  std::lock_guard lock(w.m);
  if (w.live) throw Error(409, "use_replication_stream", "live worlds stream FVP directly from the authority (finalverse.world-replication/1)");
  int changed = 0;
  if (auto s = ms_projection_refresh(w.proj, &changed); s != MS_PROJ_OK) fail(w.proj, s);
  uint8_t *buf = nullptr;
  std::size_t len = 0;
  auto s = ms_projection_events_fvp(w.proj, after, after_hash.empty() ? nullptr : after_hash.c_str(), limit, &buf, &len);
  if (s != MS_PROJ_OK) fail(w.proj, s);
  std::string out(reinterpret_cast<char *>(buf), len);
  ms_projection_free(buf);
  return out;
#else
  (void)w, (void)after, (void)after_hash, (void)limit;
  return {};
#endif
}

json::object WorldService::registration(const std::string &id) {
  World &w = get(id);
  json::object out{{"world", id}, {"authority", "mutsea"}};
#ifdef FVMAP_WITH_MUTSEA
  if (w.live) {
    out["authoritative_binding"] = nullptr;  // the stream carries no binding document; MutSea holds none for this region
  } else {
    std::lock_guard lock(w.m);
    int changed = 0;
    if (auto s = ms_projection_refresh(w.proj, &changed); s != MS_PROJ_OK) fail(w.proj, s);
    char *text = nullptr;
    std::size_t len = 0;
    auto s = ms_projection_registration_json(w.proj, &text, &len);
    out["authoritative_binding"] = take_json(w.proj, s, text, len);
  }
#endif
  if (w.config->registration) {
    std::string bytes = read_file(*w.config->registration, 1 << 20);
    boost::system::error_code ec;
    json::value doc = json::parse(bytes, ec);
    if (ec) throw Error(500, "registration_invalid", "registration document is not JSON");
    out["presentation_registration"] = json::object{{"sha256", sha256_hex(bytes)}, {"document", doc}};
  } else {
    out["presentation_registration"] = nullptr;
  }
  return out;
}

json::object WorldService::nearby(const std::string &id, double x, double y, double radius, uint32_t limit) {
  World &w = get(id);
#ifdef FVMAP_WITH_MUTSEA
  std::lock_guard lock(w.m);
  if (!w.live) throw Error(501, "live_world_required", "spatially filtered entity views need a live world");
  require_current(w);
  std::vector<ms_replica_entity> hits(limit);
  std::size_t n = 0;
  if (ms_replica_query_radius(w.live, x, y, radius, hits.data(), hits.size(), &n) != MS_REPLICA_OK)
    throw Error(400, "invalid_parameter", "x, y and radius must be finite and radius positive");
  auto st = live_status(w.live);
  json::array out;
  for (std::size_t i = 0; i < std::min<std::size_t>(n, limit); ++i) {
    const auto &e = hits[i];
    out.push_back(json::object{{"id", [&] { std::string h = hex_bytes(e.id, 16); return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20); }()},
                               {"label", e.label}, {"revision", e.revision},
                               {"position", json::array{e.position[0], e.position[1], e.position[2]}},
                               {"distance_m", std::hypot(e.position[0] - x, e.position[1] - y)}});
  }
  return json::object{{"world", w.live_world}, {"generation", hex_bytes(st.generation, 32)}, {"revision", st.revision},
                      {"frame", "region_local_z_up/metre"}, {"center", json::array{x, y}}, {"radius_m", radius},
                      {"matches", n}, {"entities", out}, {"authority", "mutsea"}, {"deterministic", true}};
#else
  (void)w, (void)x, (void)y, (void)radius, (void)limit;
  return {};
#endif
}

}  // namespace fvmap
