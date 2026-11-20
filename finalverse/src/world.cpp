#include "world.hpp"

#ifdef FVMAP_WITH_MUTSEA
#include <mutsea/projection.h>
#endif

namespace fvmap {

struct WorldService::World {
  const WorldConfig *config = nullptr;
  std::mutex m;
#ifdef FVMAP_WITH_MUTSEA
  ms_projection *proj = nullptr;
  ~World() {
    if (proj) ms_projection_close(proj);
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

}  // namespace
#endif

WorldService::WorldService(const Config &config) : config_(config) {
  for (const auto &w : config.worlds) {
    auto world = std::make_unique<World>();
    world->config = &w;
#ifdef FVMAP_WITH_MUTSEA
    ms_projection_status s = ms_projection_open(w.store.c_str(), &world->proj);
    if (s != MS_PROJ_OK)
      throw std::runtime_error("world " + w.id + ": " +
                               (world->proj ? ms_projection_last_error(world->proj) : std::string("cannot open store")));
#endif
    worlds_.push_back(std::move(world));
  }
}

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
  {
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

}  // namespace fvmap
