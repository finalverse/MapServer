// Prometheus text-format metrics.
#pragma once
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <string>

namespace fvmap {

class Metrics {
 public:
  void request(const std::string &route, int status, uint64_t duration_us);
  void backend(uint64_t duration_us, bool ok);
  void cache(bool hit) { (hit ? cache_hits_ : cache_misses_)++; }
  void rejected(const std::string &reason);
  void bytes_out(std::size_t n) { bytes_out_ += n; }
  void world_events(std::size_t n) { world_events_ += n; }
  std::string render(int backend_inflight, std::size_t cache_entries, std::size_t cache_bytes,
                     uint64_t catalog_generation) const;

 private:
  static constexpr std::array<double, 10> buckets_{0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 10};
  mutable std::mutex m_;
  std::map<std::pair<std::string, int>, uint64_t> requests_;
  std::map<std::string, std::array<uint64_t, 11>> latency_;  // per route, + overflow
  std::map<std::string, double> latency_sum_;
  std::array<uint64_t, 11> backend_hist_{};
  double backend_sum_ = 0;
  uint64_t backend_ok_ = 0, backend_fail_ = 0;
  std::map<std::string, uint64_t> rejected_;
  std::atomic<uint64_t> cache_hits_{0}, cache_misses_{0}, bytes_out_{0}, world_events_{0};
};

}  // namespace fvmap
