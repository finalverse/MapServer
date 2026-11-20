#include "server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <thread>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/url.hpp>

#include "spatial.hpp"

namespace fvmap {

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;

HttpResponse json_response(int status, const json::value &v) {
  HttpResponse r;
  r.status = status;
  r.body = json::serialize(v);
  return r;
}

HttpResponse error_response(const Error &e, const std::string &request_id) {
  return json_response(e.status, json::object{{"error", json::object{{"code", e.code}, {"message", e.what()}, {"request_id", request_id}}}});
}

Service::Service(Config config) : config_(std::move(config)), limiter_(config_.limits.rate_per_second, config_.limits.burst) {
  fs::create_directories(config_.state_dir);
  fs::permissions(config_.state_dir, fs::perms::owner_all, fs::perm_options::replace);
  tokens_ = std::make_shared<TokenStore>(config_.tokens);
  backend_ = std::make_unique<Backend>(config_.backend);
  cache_ = std::make_unique<ResponseCache>(config_.state_dir / "cache", config_.cache);
  worlds_ = std::make_unique<WorldService>(config_);
}

int Service::bind() {
  int family = config_.host.find(':') != std::string::npos ? AF_INET6 : AF_INET;
  listen_fd_ = ::socket(family, SOCK_STREAM | 0, 0);
  if (listen_fd_ < 0) throw std::runtime_error("socket");
  int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_storage ss{};
  socklen_t len = 0;
  if (family == AF_INET) {
    auto *a = reinterpret_cast<sockaddr_in *>(&ss);
    a->sin_family = AF_INET;
    a->sin_port = htons(static_cast<uint16_t>(config_.port));
    if (::inet_pton(AF_INET, config_.host.c_str(), &a->sin_addr) != 1) throw std::runtime_error("invalid listen host");
    len = sizeof *a;
  } else {
    auto *a = reinterpret_cast<sockaddr_in6 *>(&ss);
    a->sin6_family = AF_INET6;
    a->sin6_port = htons(static_cast<uint16_t>(config_.port));
    if (::inet_pton(AF_INET6, config_.host.c_str(), &a->sin6_addr) != 1) throw std::runtime_error("invalid listen host");
    len = sizeof *a;
  }
  if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&ss), len) != 0) throw std::runtime_error("cannot bind listener");
  if (::listen(listen_fd_, 128) != 0) throw std::runtime_error("cannot listen");
  len = sizeof ss;
  ::getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&ss), &len);
  port_ = ntohs(family == AF_INET ? reinterpret_cast<sockaddr_in *>(&ss)->sin_port
                                  : reinterpret_cast<sockaddr_in6 *>(&ss)->sin6_port);
  std::string host = family == AF_INET6 ? "[" + config_.host + "]" : config_.host;
  base_url_ = config_.public_base_url.value_or("http://" + host + ":" + std::to_string(port_));
  catalog_ = std::make_unique<Catalog>(config_, base_url_);
  reload();
  write_endpoint_file();
  return port_;
}

void Service::write_endpoint_file() {
  json::object o{{"schema", "finalverse.map-server-endpoint/1"}, {"endpoint", base_url_}, {"pid", static_cast<int64_t>(::getpid())},
                 {"backend", json::object{{"sha256", backend_->identity()}, {"version", backend_->version_line()}}},
                 {"started_unix_ms", started_ms_}};
  write_file_atomic(config_.state_dir / "endpoint.json", json::serialize(o));
}

void Service::reload() {
  std::lock_guard lock(reload_m_);
  auto state = catalog_->reload();
  std::map<std::string, std::string> live;
  for (auto &[id, d] : state->datasets) live[id] = d.version;
  std::size_t dropped = cache_->invalidate(live);
  {
    auto t = std::make_shared<TokenStore>(config_.tokens);
    std::lock_guard tl(tokens_m_);
    tokens_ = t;
  }
  log_event({{"event", "catalog_loaded"}, {"generation", state->generation}, {"datasets", state->datasets.size()},
             {"cache_invalidated", dropped}, {"cache_recovered", cache_->recovered()}});
}

void Service::stop() { stopping_ = true; }

void Service::serve() {
  std::vector<std::thread> workers;
  for (int i = 0; i < config_.limits.worker_threads; ++i) workers.emplace_back([this] { worker(); });
  const std::size_t max_queue = static_cast<std::size_t>(config_.limits.worker_threads) * 4;
  while (!stopping_) {
    pollfd p{listen_fd_, POLLIN, 0};
    int n = ::poll(&p, 1, 200);
    if (n <= 0) continue;
    int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) continue;
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    timeval tv{config_.limits.io_timeout_ms / 1000, (config_.limits.io_timeout_ms % 1000) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    std::unique_lock lock(q_m_);
    if (queue_.size() >= max_queue) {
      lock.unlock();
      metrics_.rejected("overloaded");
      static const char busy[] =
          "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: 63\r\n\r\n"
          "{\"error\":{\"code\":\"overloaded\",\"message\":\"connection queue full\"}}";
      (void)!::write(fd, busy, sizeof busy - 1);
      ::close(fd);
      continue;
    }
    queue_.push_back(fd);
    q_cv_.notify_one();
  }
  q_cv_.notify_all();
  for (auto &t : workers) t.join();
  ::close(listen_fd_);
  log_event({{"event", "stopped"}});
}

void Service::worker() {
  for (;;) {
    int fd;
    {
      std::unique_lock lock(q_m_);
      q_cv_.wait_for(lock, std::chrono::milliseconds(200), [&] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) {
        if (stopping_) return;
        continue;
      }
      fd = queue_.front();
      queue_.pop_front();
    }
    try {
      connection(fd);
    } catch (...) {
    }
    ::close(fd);
  }
}

void Service::connection(int fd) {
  asio::io_context io;
  asio::ip::tcp::socket sock(io);
  boost::system::error_code ec;
  sock.assign(config_.host.find(':') != std::string::npos ? asio::ip::tcp::v6() : asio::ip::tcp::v4(), ::dup(fd), ec);
  if (ec) return;
  beast::flat_buffer buf(config_.limits.max_request_bytes + 1024);
  for (int served = 0; served < config_.limits.max_keepalive_requests && !stopping_; ++served) {
    http::request_parser<http::string_body> parser;
    parser.header_limit(static_cast<std::uint32_t>(config_.limits.max_request_bytes));
    parser.body_limit(64 * 1024);
    http::read(sock, buf, parser, ec);
    if (ec == http::error::end_of_stream || ec == asio::error::eof || ec == asio::error::connection_reset) return;
    uint64_t t0 = monotonic_us();
    HttpResponse out;
    std::string route_name = "malformed", principal = "-";
    HttpRequest req;
    req.request_id = random_hex(8);
    bool keep = false;
    if (ec) {
      if (ec == asio::error::timed_out || ec == asio::error::would_block || ec == asio::error::try_again) return;
      metrics_.rejected("malformed_http");
      out = error_response(Error(ec == http::error::header_limit ? 431 : 400, "malformed_http", "request could not be parsed"), req.request_id);
    } else {
      auto &m = parser.get();
      req.method = std::string(m.method_string());
      req.target = std::string(m.target());
      req.authorization = std::string(m[http::field::authorization]);
      keep = m.keep_alive();
      try {
        out = route(req, route_name, principal);
      } catch (const Error &e) {
        out = error_response(e, req.request_id);
      } catch (const std::exception &e) {
        log_event({{"event", "internal_error"}, {"request_id", req.request_id}, {"detail", e.what()}});
        out = error_response(Error(500, "internal", "internal error"), req.request_id);
      }
    }
    http::response<http::string_body> res{static_cast<http::status>(out.status), 11};
    res.set(http::field::server, "Finalverse-Map-Server/0.1");
    res.set(http::field::content_type, out.content_type);
    res.set("X-Request-Id", req.request_id);
    res.set("X-Content-Type-Options", "nosniff");
    for (auto &[k, v] : out.headers) res.set(k, v);
    res.keep_alive(keep && out.status < 500);
    res.body() = std::move(out.body);
    res.prepare_payload();
    std::size_t bytes = res.body().size();
    http::write(sock, res, ec);
    uint64_t us = monotonic_us() - t0;
    metrics_.request(route_name, out.status, us);
    metrics_.bytes_out(bytes);
    std::string cache_state;
    for (auto &[k, v] : out.headers)
      if (k == "X-Cache") cache_state = v;
    log_event({{"event", "request"}, {"request_id", req.request_id}, {"principal", principal}, {"method", req.method},
               {"route", route_name}, {"status", out.status}, {"bytes", bytes}, {"us", us}, {"cache", cache_state}});
    if (ec || !res.keep_alive()) return;
  }
}

HttpResponse Service::handle(const HttpRequest &r) {
  std::string route_name, principal;
  try {
    return route(r, route_name, principal);
  } catch (const Error &e) {
    return error_response(e, r.request_id);
  }
}

namespace {

std::vector<std::string> split_path(std::string_view path) {
  std::vector<std::string> parts;
  std::size_t start = 1;
  while (start <= path.size()) {
    std::size_t end = path.find('/', start);
    parts.emplace_back(path.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return parts;
}

uint64_t parse_u64(const std::string &s, const char *what) {
  if (s.empty() || s.size() > 19 || s.find_first_not_of("0123456789") != std::string::npos)
    throw Error(400, "invalid_parameter", std::string(what) + " must be a non-negative integer");
  return std::stoull(s);
}

}  // namespace

HttpResponse Service::route(const HttpRequest &r, std::string &route_name, std::string &principal_name) {
  if (r.target.size() > 4096 || r.target.empty() || r.target[0] != '/') throw Error(400, "malformed_target", "request target is invalid");
  std::string_view target = r.target;
  std::string_view path = target.substr(0, target.find('?'));
  std::string_view query = path.size() < target.size() ? target.substr(path.size() + 1) : std::string_view{};
  // Paths are matched literally: no decoding, no dot segments, no doubled slashes.
  if (path.find("..") != std::string_view::npos || path.find("//") != std::string_view::npos ||
      path.find('%') != std::string_view::npos || path.find('\\') != std::string_view::npos)
    throw Error(400, "malformed_target", "path segments must be literal");
  auto parts = split_path(path);
  if (r.method != "GET" && r.method != "POST") throw Error(405, "method_not_allowed", "method not allowed");
  bool get = r.method == "GET";

  if (path == "/healthz") {
    route_name = "healthz";
    return json_response(200, json::object{{"status", "ok"}, {"service", "finalverse-map-server"}});
  }
  if (path == "/readyz") {
    route_name = "readyz";
    json::object checks{{"backend", backend_->healthy()}};
    bool ready = backend_->healthy();
    auto state = catalog_->current();
    json::object ds;
    for (auto &[id, d] : state->datasets) {
      bool fresh = !catalog_->stale(d);
      ds[id] = fresh ? "current" : "changed";
      ready &= fresh;
    }
    checks["datasets"] = ds;
    return json_response(ready ? 200 : 503, json::object{{"ready", ready}, {"checks", checks}});
  }

  route_name = "unauthorized";
  std::shared_ptr<const TokenStore> tokens;
  {
    std::lock_guard tl(tokens_m_);
    tokens = tokens_;
  }
  auto principal = tokens->authenticate(r.authorization);
  if (!principal) {
    metrics_.rejected("unauthenticated");
    HttpResponse e = error_response(Error(401, "unauthenticated", "a valid bearer token is required"), r.request_id);
    e.headers.emplace_back("WWW-Authenticate", "Bearer realm=\"finalverse-map-server\"");
    return e;
  }
  principal_name = principal->name;
  if (!limiter_.allow(principal->name)) {
    metrics_.rejected("rate_limited");
    HttpResponse e = error_response(Error(429, "rate_limited", "request rate exceeds the principal's allowance"), r.request_id);
    e.headers.emplace_back("Retry-After", "1");
    return e;
  }
  auto require_admin = [&] {
    if (principal->role != Role::Admin) throw Error(403, "forbidden", "administrator role required");
  };
  if (path == "/metrics" && get) {
    route_name = "metrics";
    require_admin();
    HttpResponse m;
    m.content_type = "text/plain; version=0.0.4";
    m.body = metrics_.render(backend_->inflight(), cache_->entries(), cache_->bytes(), catalog_->current()->generation);
    return m;
  }
  if (parts.size() >= 2 && parts[0] == "v1") {
    const std::string &area = parts[1];
    if (area == "catalog" && get) {
      route_name = "catalog";
      auto state = catalog_->current();
      if (parts.size() == 2) {
        json::array out;
        for (auto &[id, d] : state->datasets)
          if (principal->may_read_dataset(id)) out.push_back(catalog_->describe(d));
        return json_response(200, json::object{{"schema", "finalverse.map-catalog/2"}, {"generation", state->generation},
                                               {"backend", json::object{{"engine", "MapServer"}, {"version", backend_->version_line()}, {"sha256", backend_->identity()}}},
                                               {"datasets", out}});
      }
      if (parts.size() == 3) {
        auto it = state->datasets.find(parts[2]);
        if (it == state->datasets.end() || !principal->may_read_dataset(parts[2]))
          throw Error(404, "unknown_dataset", "dataset is not available");
        return json_response(200, catalog_->describe(it->second));
      }
    }
    if (area == "maps" && get && parts.size() == 4 && parts[3] == "wms") {
      route_name = "wms";
      return maps(*principal, parts[2], query);
    }
    if (area == "spatial" && get && parts.size() == 4) {
      auto state = catalog_->current();
      auto it = state->datasets.find(parts[2]);
      if (it == state->datasets.end() || !principal->may_read_dataset(parts[2]))
        throw Error(404, "unknown_dataset", "dataset is not available");
      if (catalog_->stale(it->second)) throw Error(409, "dataset_changed", "dataset sources changed; reload required");
      if (parts[3] == "inspect") {
        route_name = "spatial_inspect";
        return json_response(200, spatial_inspect(it->second, parse_inspect(query, it->second)));
      }
      if (parts[3] == "nearby") {
        route_name = "spatial_nearby";
        return json_response(200, spatial_nearby(it->second, parse_nearby(query, it->second)));
      }
    }
    if (area == "worlds" && get) {
      route_name = "worlds";
      if (parts.size() == 2) {
        json::array visible;
        for (auto &w : worlds_->list())
          if (principal->may_read_world(std::string(w.as_object().at("id").as_string()))) visible.push_back(w);
        return json_response(200, json::object{{"worlds", visible}, {"projection", worlds_->enabled()}});
      }
      const std::string &wid = parts[2];
      if (!worlds_->find(wid) || !principal->may_read_world(wid)) throw Error(404, "unknown_world", "world is not available");
      const std::string query_text(query);  // owned, NUL-terminated
      auto params = boost::urls::parse_query(query_text);
      if (!params) throw Error(400, "malformed_query", "query string is not well formed");
      std::map<std::string, std::string> q;
      for (auto p : *params) {
        std::string k = p.key.decode();
        if (k != "after" && k != "after_hash" && k != "limit") throw Error(400, "unsupported_parameter", "parameter is not accepted: " + k);
        if (!q.emplace(k, p.value.decode()).second) throw Error(400, "duplicate_parameter", "parameter repeated");
      }
      if (parts.size() == 3) {
        route_name = "world_head";
        return json_response(200, worlds_->head(wid));
      }
      if (parts.size() == 4 && parts[3] == "snapshot") {
        route_name = "world_snapshot";
        return json_response(200, worlds_->snapshot(wid));
      }
      if (parts.size() == 4 && parts[3] == "registration") {
        route_name = "world_registration";
        return json_response(200, worlds_->registration(wid));
      }
      if (parts.size() == 4 && (parts[3] == "events" || parts[3] == "stream")) {
        uint64_t after = q.contains("after") ? parse_u64(q["after"], "after") : 0;
        uint64_t limit = q.contains("limit") ? parse_u64(q["limit"], "limit") : 256;
        if (limit < 1 || limit > 1024) throw Error(400, "invalid_parameter", "limit must be 1..1024");
        std::string hash = q.contains("after_hash") ? q["after_hash"] : "";
        if (!hash.empty() && (hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos))
          throw Error(400, "invalid_parameter", "after_hash must be 64 lowercase hex characters");
        if (parts[3] == "events") {
          route_name = "world_events";
          auto ev = worlds_->events(wid, after, hash, static_cast<uint32_t>(limit));
          if (auto *a = ev.if_contains("events"); a && a->is_array()) metrics_.world_events(a->as_array().size());
          return json_response(200, ev);
        }
        route_name = "world_stream";
        HttpResponse s;
        s.content_type = "application/x-finalverse-fvp";
        s.body = worlds_->events_fvp(wid, after, hash, static_cast<uint32_t>(limit));
        return s;
      }
    }
    if (area == "admin" && r.method == "POST") {
      require_admin();
      if (parts.size() == 3 && parts[2] == "reload") {
        route_name = "admin_reload";
        reload();
        return json_response(200, json::object{{"generation", catalog_->current()->generation}, {"datasets", catalog_->list()}});
      }
      if (parts.size() == 4 && parts[2] == "cache" && parts[3] == "purge") {
        route_name = "admin_cache_purge";
        return json_response(200, json::object{{"purged", cache_->purge_all()}});
      }
    }
  }
  route_name = "not_found";
  throw Error(404, "not_found", "no such resource");
}

HttpResponse Service::maps(const Principal &p, const std::string &dataset_ref, std::string_view query) {
  std::string id = dataset_ref, pinned;
  if (auto at = dataset_ref.find('@'); at != std::string::npos) {
    id = dataset_ref.substr(0, at);
    pinned = dataset_ref.substr(at + 1);
  }
  auto state = catalog_->current();
  auto it = state->datasets.find(id);
  if (it == state->datasets.end() || !p.may_read_dataset(id)) throw Error(404, "unknown_dataset", "dataset is not available");
  const DatasetState &d = it->second;
  if (!pinned.empty() && pinned != d.version)
    throw Error(410, "version_retired", "the requested dataset version is not the current published version");
  if (catalog_->stale(d)) {
    metrics_.rejected("dataset_changed");
    throw Error(409, "dataset_changed", "dataset sources changed; an operator reload is required");
  }
  WmsRequest w = validate_wms(query, d, config_.limits, extents_);
  std::string canonical = w.canonical_query();
  std::string key = ResponseCache::make_key(id, d.version, backend_->identity(), canonical);
  HttpResponse out;
  out.headers = {{"ETag", "\"" + key + "\""}, {"X-Dataset-Version", d.version}, {"Cache-Control", "private, max-age=300"}};
  if (auto hit = cache_->get(key)) {
    metrics_.cache(true);
    out.content_type = hit->content_type;
    out.body = std::move(hit->body);
    out.headers.emplace_back("X-Cache", "HIT");
    return out;
  }
  metrics_.cache(false);
  BackendResult b;
  try {
    b = backend_->run(state->mapserver_config.string(), d.map_alias, canonical, std::to_string(port_), config_.limits.queue_timeout_ms);
    metrics_.backend(b.duration_us, true);
  } catch (const Error &e) {
    metrics_.backend(0, false);
    throw;
  }
  bool image = b.content_type.starts_with("image/");
  bool xml = b.content_type.find("xml") != std::string::npos;
  if (b.cgi_status != 200 || (w.is_map() && (!image || b.content_type.rfind(w.format, 0) != 0)) || (!w.is_map() && !xml)) {
    // MapServer service exceptions can include host paths; log them, return a code.
    log_event({{"event", "backend_exception"}, {"dataset", id}, {"content_type", b.content_type},
               {"detail", b.body.substr(0, 400)}});
    throw Error(502, "backend_exception", "map engine reported an exception for this request");
  }
  cache_->put(key, id, d.version, b.content_type, b.body);
  out.content_type = b.content_type;
  out.body = std::move(b.body);
  out.headers.emplace_back("X-Cache", "MISS");
  return out;
}

}  // namespace fvmap
