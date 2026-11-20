#include "metrics.hpp"

#include <sstream>

namespace fvmap {

namespace {
std::size_t bucket(double s, const std::array<double, 10> &b) {
  for (std::size_t i = 0; i < b.size(); ++i)
    if (s <= b[i]) return i;
  return b.size();
}
}  // namespace

void Metrics::request(const std::string &route, int status, uint64_t us) {
  std::lock_guard lock(m_);
  requests_[{route, status}]++;
  double s = us / 1e6;
  latency_[route][bucket(s, buckets_)]++;
  latency_sum_[route] += s;
}

void Metrics::backend(uint64_t us, bool ok) {
  std::lock_guard lock(m_);
  double s = us / 1e6;
  backend_hist_[bucket(s, buckets_)]++;
  backend_sum_ += s;
  (ok ? backend_ok_ : backend_fail_)++;
}

void Metrics::rejected(const std::string &reason) {
  std::lock_guard lock(m_);
  rejected_[reason]++;
}

std::string Metrics::render(int inflight, std::size_t entries, std::size_t bytes, uint64_t generation) const {
  std::lock_guard lock(m_);
  std::ostringstream o;
  o << "# TYPE fvmap_requests_total counter\n";
  for (auto &[k, v] : requests_)
    o << "fvmap_requests_total{route=\"" << k.first << "\",status=\"" << k.second << "\"} " << v << "\n";
  o << "# TYPE fvmap_request_seconds histogram\n";
  for (auto &[route, h] : latency_) {
    uint64_t cum = 0;
    for (std::size_t i = 0; i < buckets_.size(); ++i) {
      cum += h[i];
      o << "fvmap_request_seconds_bucket{route=\"" << route << "\",le=\"" << buckets_[i] << "\"} " << cum << "\n";
    }
    cum += h[buckets_.size()];
    o << "fvmap_request_seconds_bucket{route=\"" << route << "\",le=\"+Inf\"} " << cum << "\n";
    o << "fvmap_request_seconds_sum{route=\"" << route << "\"} " << latency_sum_.at(route) << "\n";
    o << "fvmap_request_seconds_count{route=\"" << route << "\"} " << cum << "\n";
  }
  o << "# TYPE fvmap_backend_seconds histogram\n";
  uint64_t cum = 0;
  for (std::size_t i = 0; i < buckets_.size(); ++i) {
    cum += backend_hist_[i];
    o << "fvmap_backend_seconds_bucket{le=\"" << buckets_[i] << "\"} " << cum << "\n";
  }
  cum += backend_hist_[buckets_.size()];
  o << "fvmap_backend_seconds_bucket{le=\"+Inf\"} " << cum << "\nfvmap_backend_seconds_sum " << backend_sum_
    << "\nfvmap_backend_seconds_count " << cum << "\n";
  o << "# TYPE fvmap_backend_results_total counter\nfvmap_backend_results_total{result=\"ok\"} " << backend_ok_
    << "\nfvmap_backend_results_total{result=\"error\"} " << backend_fail_ << "\n";
  o << "# TYPE fvmap_rejected_total counter\n";
  for (auto &[k, v] : rejected_) o << "fvmap_rejected_total{reason=\"" << k << "\"} " << v << "\n";
  o << "# TYPE fvmap_cache_requests_total counter\nfvmap_cache_requests_total{result=\"hit\"} " << cache_hits_
    << "\nfvmap_cache_requests_total{result=\"miss\"} " << cache_misses_ << "\n";
  o << "# TYPE fvmap_cache_entries gauge\nfvmap_cache_entries " << entries << "\n";
  o << "# TYPE fvmap_cache_bytes gauge\nfvmap_cache_bytes " << bytes << "\n";
  o << "# TYPE fvmap_backend_inflight gauge\nfvmap_backend_inflight " << inflight << "\n";
  o << "# TYPE fvmap_response_bytes_total counter\nfvmap_response_bytes_total " << bytes_out_ << "\n";
  o << "# TYPE fvmap_world_events_total counter\nfvmap_world_events_total " << world_events_ << "\n";
  o << "# TYPE fvmap_catalog_generation gauge\nfvmap_catalog_generation " << generation << "\n";
  return o.str();
}

}  // namespace fvmap
