// Executes the unmodified MapServer CGI binary under strict bounds.
#pragma once
#include <atomic>
#include <semaphore>
#include <string>

#include "config.hpp"

namespace fvmap {

struct BackendResult {
  int cgi_status = 200;
  std::string content_type;
  std::string body;
  uint64_t duration_us = 0;
};

class Backend {
 public:
  explicit Backend(const BackendConfig &config);
  // Runs one request. `query` must already be validated and canonical; `alias`
  // selects the mapfile through the MapServer configuration file.
  BackendResult run(const std::string &mapserver_config, const std::string &alias,
                    const std::string &query, const std::string &server_port,
                    int queue_timeout_ms, const std::atomic<bool> *cancel = nullptr);
  const std::string &identity() const { return sha256_; }
  std::string version_line() const { return version_; }
  bool healthy() const;
  int inflight() const { return inflight_.load(); }

 private:
  const BackendConfig &config_;
  std::string sha256_, version_;
  std::counting_semaphore<64> slots_;
  std::atomic<int> inflight_{0};
};

}  // namespace fvmap
