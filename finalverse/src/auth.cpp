#include "auth.hpp"

#include <sys/stat.h>

namespace fvmap {

TokenStore TokenStore::from_json(const json::value &root) {
  const auto &o = as_object(root, "token file");
  reject_unknown(o, {"schema", "principals"}, "token file");
  if (get_string(o, "schema") != "finalverse.map-server-tokens/1")
    throw std::runtime_error("unsupported token file schema");
  TokenStore store;
  std::set<std::string> names, digests;
  for (auto &pv : o.at("principals").as_array()) {
    const auto &p = as_object(pv, "principal");
    reject_unknown(p, {"principal", "token_sha256", "role", "datasets", "worlds"}, "principal");
    Entry e;
    e.principal.name = get_string(p, "principal");
    if (!valid_identifier(e.principal.name) || !names.insert(e.principal.name).second)
      throw std::runtime_error("invalid or duplicate principal");
    e.digest = get_string(p, "token_sha256");
    if (e.digest.size() != 64 || e.digest.find_first_not_of("0123456789abcdef") != std::string::npos ||
        !digests.insert(e.digest).second)
      throw std::runtime_error("token_sha256 must be unique lowercase hex");
    std::string role = get_string(p, "role");
    if (role == "reader") e.principal.role = Role::Reader;
    else if (role == "admin") e.principal.role = Role::Admin;
    else throw std::runtime_error("unknown role");
    for (const char *key : {"datasets", "worlds"}) {
      auto it = p.find(key);
      if (it == p.end()) continue;
      auto &set = std::string_view(key) == "datasets" ? e.principal.datasets : e.principal.worlds;
      for (auto &d : it->value().as_array()) {
        std::string s(d.as_string());
        if (s != "*" && !valid_identifier(s)) throw std::runtime_error("invalid scope entry");
        set.insert(s);
      }
    }
    store.entries_.push_back(std::move(e));
  }
  if (store.entries_.empty()) throw std::runtime_error("token file has no principals");
  return store;
}

TokenStore::TokenStore(const fs::path &path) {
  struct stat st{};
  if (::lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
    throw std::runtime_error("token file must be a regular file");
  if (st.st_mode & (S_IRWXG | S_IRWXO)) throw std::runtime_error("token file must be private (0600)");
  boost::system::error_code ec;
  json::value v = json::parse(read_file(path, 1 << 20), ec);
  if (ec) throw std::runtime_error("token file is not valid JSON");
  *this = from_json(v);
}

std::optional<Principal> TokenStore::authenticate(std::string_view header) const {
  constexpr std::string_view prefix = "Bearer ";
  if (!header.starts_with(prefix)) return std::nullopt;
  std::string_view token = header.substr(prefix.size());
  if (token.size() < 32 || token.size() > 256) return std::nullopt;
  std::string digest = sha256_hex(token);
  const Entry *found = nullptr;
  // Compare against every entry so timing does not reveal the position.
  for (auto &e : entries_)
    if (constant_time_equal(e.digest, digest)) found = &e;
  if (!found) return std::nullopt;
  return found->principal;
}

bool RateLimiter::allow(const std::string &principal) {
  std::lock_guard lock(m_);
  uint64_t now = monotonic_us();
  auto [it, inserted] = buckets_.try_emplace(principal, Bucket{burst_, now});
  Bucket &b = it->second;
  if (!inserted) {
    b.tokens = std::min(burst_, b.tokens + (now - b.last_us) * rate_ / 1e6);
    b.last_us = now;
  }
  if (b.tokens < 1.0) return false;
  b.tokens -= 1.0;
  return true;
}

}  // namespace fvmap
