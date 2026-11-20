// Bearer-token access control with per-dataset and per-world scopes.
#pragma once
#include <chrono>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "util.hpp"

namespace fvmap {

enum class Role { Reader, Admin };

struct Principal {
  std::string name;
  Role role = Role::Reader;
  std::set<std::string> datasets;  // "*" grants all
  std::set<std::string> worlds;    // "*" grants all
  bool may_read_dataset(const std::string &id) const {
    return datasets.contains("*") || datasets.contains(id);
  }
  bool may_read_world(const std::string &id) const {
    return worlds.contains("*") || worlds.contains(id);
  }
};

// Token files hold SHA-256 digests only (finalverse.map-server-tokens/1) and
// must not be readable by group or others.
class TokenStore {
 public:
  explicit TokenStore(const fs::path &path);
  static TokenStore from_json(const json::value &v);
  // Returns the principal for an "Authorization: Bearer <token>" header value.
  std::optional<Principal> authenticate(std::string_view header) const;
  std::size_t size() const { return entries_.size(); }

 private:
  TokenStore() = default;
  struct Entry {
    std::string digest;
    Principal principal;
  };
  std::vector<Entry> entries_;
};

// Token bucket per principal.
class RateLimiter {
 public:
  RateLimiter(double rate, double burst) : rate_(rate), burst_(burst) {}
  bool allow(const std::string &principal);

 private:
  struct Bucket {
    double tokens;
    uint64_t last_us;
  };
  double rate_, burst_;
  std::mutex m_;
  std::unordered_map<std::string, Bucket> buckets_;
};

}  // namespace fvmap
