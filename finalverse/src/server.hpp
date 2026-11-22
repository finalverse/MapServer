// HTTP/1.1 front end (Boost.Beast) and request router.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include "auth.hpp"
#include "backend.hpp"
#include "cache.hpp"
#include "catalog.hpp"
#include "metrics.hpp"
#include "wms.hpp"
#include "world.hpp"

namespace fvmap {

struct HttpRequest {
  std::string method, target, authorization, request_id;
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
};

class Service {
 public:
  explicit Service(Config config);
  // Binds the listener; returns the bound port (useful with port 0).
  int bind();
  void serve();  // blocks until stop()
  void stop();
  void reload();  // re-hash sources, regenerate mapfiles, invalidate cache, reload tokens
  HttpResponse handle(const HttpRequest &r);  // routing, exposed for tests
  int port() const { return port_; }

 private:
  Config config_;
  std::string base_url_;
  std::unique_ptr<Catalog> catalog_;
  std::unique_ptr<Backend> backend_;
  std::unique_ptr<ResponseCache> cache_;
  std::unique_ptr<WorldService> worlds_;
  std::shared_ptr<const TokenStore> tokens_;
  std::mutex tokens_m_, reload_m_;
  RateLimiter limiter_;
  Metrics metrics_;
  ExtentIndex extents_;
  int listen_fd_ = -1, port_ = 0;
  std::atomic<bool> stopping_{false};
  std::mutex q_m_;
  std::condition_variable q_cv_;
  std::deque<int> queue_;
  int64_t started_ms_ = unix_ms();

  void worker();
  void connection(int fd);
  HttpResponse route(const HttpRequest &r, std::string &route_name, std::string &principal_name);
  HttpResponse maps(const Principal &p, const std::string &dataset_ref, std::string_view query);
  void write_endpoint_file();
};

HttpResponse json_response(int status, const json::value &v);
HttpResponse error_response(const Error &e, const std::string &request_id);

}  // namespace fvmap
