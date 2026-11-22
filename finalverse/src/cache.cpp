#include "cache.hpp"

namespace fvmap {

ResponseCache::ResponseCache(fs::path dir, CacheConfig config) : dir_(std::move(dir)), config_(config) {
  fs::create_directories(dir_);
  fs::permissions(dir_, fs::perms::owner_all, fs::perm_options::replace);
  // Restart recovery: rebuild the index from verified metadata. Anything that
  // does not verify is removed instead of served.
  std::vector<std::pair<int64_t, std::string>> found;
  for (auto &entry : fs::recursive_directory_iterator(dir_)) {
    if (!entry.is_regular_file()) continue;
    auto p = entry.path();
    if (p.filename().string().find(".tmp-") != std::string::npos) {
      fs::remove(p);
      continue;
    }
    if (p.extension() != ".meta") continue;
    std::string key = p.stem().string();
    try {
      boost::system::error_code ec;
      json::value v = json::parse(read_file(p, 4096), ec);
      if (ec) throw std::runtime_error("meta");
      auto &o = v.as_object();
      if (o.at("schema") != "finalverse.map-cache-entry/1" || o.at("key").as_string() != key) throw std::runtime_error("meta");
      Entry e;
      e.dataset = o.at("dataset").as_string();
      e.version = o.at("version").as_string();
      e.content_type = o.at("content_type").as_string();
      e.body_sha256 = o.at("body_sha256").as_string();
      e.size = static_cast<std::size_t>(o.at("size").to_number<uint64_t>());
      if (!fs::exists(body_path(key)) || fs::file_size(body_path(key)) != e.size) throw std::runtime_error("body");
      found.emplace_back(o.at("created_unix_ms").to_number<int64_t>(), key);
      index_[key] = e;
      bytes_ += e.size;
    } catch (...) {
      std::error_code ec;
      fs::remove(p, ec);
      fs::remove(body_path(key), ec);
    }
  }
  std::sort(found.begin(), found.end());
  for (auto &[t, key] : found) {
    lru_.push_front(key);
    index_[key].lru = lru_.begin();
  }
  recovered_ = index_.size();
  std::lock_guard lock(m_);
  evict_locked();
}

std::string ResponseCache::make_key(const std::string &dataset, const std::string &version,
                                    const std::string &backend, const std::string &canonical_query) {
  std::string basis = "finalverse.map-cache/1";
  for (auto *part : {&dataset, &version, &backend, &canonical_query}) {
    basis.push_back('\0');
    basis += *part;
  }
  return sha256_hex(basis);
}

fs::path ResponseCache::body_path(const std::string &key) const { return dir_ / key.substr(0, 2) / (key + ".body"); }
fs::path ResponseCache::meta_path(const std::string &key) const { return dir_ / key.substr(0, 2) / (key + ".meta"); }

std::optional<CachedResponse> ResponseCache::get(const std::string &key) {
  Entry e;
  {
    std::lock_guard lock(m_);
    auto it = index_.find(key);
    if (it == index_.end()) return std::nullopt;
    lru_.splice(lru_.begin(), lru_, it->second.lru);
    e = it->second;
  }
  std::string body;
  try {
    body = read_file(body_path(key), e.size);
  } catch (...) {
    std::lock_guard lock(m_);
    erase_locked(key);
    return std::nullopt;
  }
  if (body.size() != e.size || sha256_hex(body) != e.body_sha256) {
    std::lock_guard lock(m_);
    erase_locked(key);  // corrupted on disk: never served
    return std::nullopt;
  }
  return CachedResponse{e.content_type, std::move(body), key};
}

void ResponseCache::put(const std::string &key, const std::string &dataset, const std::string &version,
                        const std::string &content_type, const std::string &body) {
  if (config_.max_entries == 0 || body.size() > config_.max_bytes / 4) return;
  {
    // Concurrent misses for one key: the first writer wins.
    std::lock_guard lock(m_);
    if (index_.contains(key)) return;
  }
  fs::create_directories(dir_ / key.substr(0, 2));
  std::string digest = sha256_hex(body);
  write_file_atomic(body_path(key), body);
  json::object meta{{"schema", "finalverse.map-cache-entry/1"}, {"key", key},         {"dataset", dataset},
                    {"version", version},                       {"content_type", content_type},
                    {"body_sha256", digest},                    {"size", body.size()}, {"created_unix_ms", unix_ms()}};
  write_file_atomic(meta_path(key), json::serialize(meta));
  std::lock_guard lock(m_);
  if (index_.contains(key)) return;  // identical bytes were written by the racing writer
  lru_.push_front(key);
  index_[key] = Entry{dataset, version, content_type, digest, body.size(), lru_.begin()};
  bytes_ += body.size();
  evict_locked();
}

void ResponseCache::erase_locked(const std::string &key_ref) {
  const std::string key = key_ref;  // the reference may point into lru_
  auto it = index_.find(key);
  if (it == index_.end()) return;
  bytes_ -= it->second.size;
  lru_.erase(it->second.lru);
  index_.erase(it);
  std::error_code ec;
  fs::remove(body_path(key), ec);
  fs::remove(meta_path(key), ec);
}

void ResponseCache::evict_locked() {
  while (!lru_.empty() && (bytes_ > config_.max_bytes || index_.size() > config_.max_entries))
    erase_locked(lru_.back());
}

std::size_t ResponseCache::invalidate(const std::map<std::string, std::string> &live) {
  std::lock_guard lock(m_);
  std::vector<std::string> doomed;
  for (auto &[k, e] : index_) {
    auto it = live.find(e.dataset);
    if (it == live.end() || it->second != e.version) doomed.push_back(k);
  }
  for (auto &k : doomed) erase_locked(k);
  return doomed.size();
}

std::size_t ResponseCache::purge_all() {
  std::lock_guard lock(m_);
  std::size_t n = index_.size();
  while (!lru_.empty()) erase_locked(lru_.back());
  return n;
}

std::size_t ResponseCache::entries() const {
  std::lock_guard lock(m_);
  return index_.size();
}
std::size_t ResponseCache::bytes() const {
  std::lock_guard lock(m_);
  return bytes_;
}

}  // namespace fvmap
