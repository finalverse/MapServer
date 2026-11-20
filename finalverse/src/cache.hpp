// Persistent, content-verified response cache with LRU eviction.
//
// key = SHA-256("finalverse.map-cache/1" | dataset | dataset version |
//               backend sha256 | canonical query)
// so a changed source, configuration, or engine binary can never be served a
// stale entry. Entries survive restarts; invalidate() removes every entry
// whose dataset version is no longer current.
#pragma once
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>

#include "config.hpp"

namespace fvmap {

struct CachedResponse {
  std::string content_type, body, key;
};

class ResponseCache {
 public:
  ResponseCache(fs::path dir, CacheConfig config);
  static std::string make_key(const std::string &dataset, const std::string &version,
                              const std::string &backend, const std::string &canonical_query);
  std::optional<CachedResponse> get(const std::string &key);
  void put(const std::string &key, const std::string &dataset, const std::string &version,
           const std::string &content_type, const std::string &body);
  // Drops entries not matching the given live dataset versions; returns count.
  std::size_t invalidate(const std::map<std::string, std::string> &live_versions);
  std::size_t purge_all();
  std::size_t entries() const;
  std::size_t bytes() const;
  std::size_t recovered() const { return recovered_; }

 private:
  struct Entry {
    std::string dataset, version, content_type, body_sha256;
    std::size_t size;
    std::list<std::string>::iterator lru;
  };
  fs::path dir_;
  CacheConfig config_;
  mutable std::mutex m_;
  std::unordered_map<std::string, Entry> index_;
  std::list<std::string> lru_;  // front = most recent
  std::size_t bytes_ = 0, recovered_ = 0;
  fs::path body_path(const std::string &key) const;
  fs::path meta_path(const std::string &key) const;
  void erase_locked(const std::string &key);
  void evict_locked();
};

}  // namespace fvmap
