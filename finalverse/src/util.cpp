#include "util.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace fvmap {

std::string hex(const unsigned char *data, std::size_t size) {
  static const char *digits = "0123456789abcdef";
  std::string out(size * 2, '0');
  for (std::size_t i = 0; i < size; ++i) {
    out[2 * i] = digits[data[i] >> 4];
    out[2 * i + 1] = digits[data[i] & 15];
  }
  return out;
}

namespace {
struct Digest {
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  Digest() {
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1)
      throw std::runtime_error("sha256 unavailable");
  }
  ~Digest() { EVP_MD_CTX_free(ctx); }
  void update(const void *p, std::size_t n) {
    if (EVP_DigestUpdate(ctx, p, n) != 1) throw std::runtime_error("sha256 update");
  }
  std::string finish() {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    if (EVP_DigestFinal_ex(ctx, md, &n) != 1) throw std::runtime_error("sha256 final");
    return hex(md, n);
  }
};
}  // namespace

std::string sha256_hex(std::string_view bytes) {
  Digest d;
  d.update(bytes.data(), bytes.size());
  return d.finish();
}

std::string sha256_file_hex(const fs::path &path) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) throw Error(500, "source_unreadable", "dataset source cannot be read");
  Digest d;
  char buf[1 << 16];
  for (;;) {
    ssize_t n = ::read(fd, buf, sizeof buf);
    if (n < 0) {
      ::close(fd);
      throw Error(500, "source_unreadable", "dataset source cannot be read");
    }
    if (n == 0) break;
    d.update(buf, static_cast<std::size_t>(n));
  }
  ::close(fd);
  return d.finish();
}

bool constant_time_equal(std::string_view a, std::string_view b) {
  unsigned char diff = a.size() == b.size() ? 0 : 1;
  std::size_t n = std::max(a.size(), b.size());
  for (std::size_t i = 0; i < n; ++i) {
    unsigned char x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
    diff |= x ^ y;
  }
  return diff == 0;
}

std::string random_hex(std::size_t bytes) {
  std::vector<unsigned char> buf(bytes);
  if (RAND_bytes(buf.data(), static_cast<int>(bytes)) != 1) throw std::runtime_error("rng");
  return hex(buf.data(), bytes);
}

std::string read_file(const fs::path &path, std::size_t max_bytes) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) throw std::runtime_error("cannot open " + path.filename().string());
  std::string out;
  char buf[1 << 16];
  for (;;) {
    ssize_t n = ::read(fd, buf, sizeof buf);
    if (n < 0) {
      ::close(fd);
      throw std::runtime_error("cannot read " + path.filename().string());
    }
    if (n == 0) break;
    out.append(buf, static_cast<std::size_t>(n));
    if (out.size() > max_bytes) {
      ::close(fd);
      throw std::runtime_error(path.filename().string() + " exceeds its size bound");
    }
  }
  ::close(fd);
  return out;
}

void write_file_atomic(const fs::path &path, std::string_view bytes) {
  fs::path tmp = path;
  tmp += ".tmp-" + random_hex(6);
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) throw std::runtime_error("cannot create state file");
  std::size_t off = 0;
  while (off < bytes.size()) {
    ssize_t n = ::write(fd, bytes.data() + off, bytes.size() - off);
    if (n <= 0) {
      ::close(fd);
      ::unlink(tmp.c_str());
      throw std::runtime_error("cannot write state file");
    }
    off += static_cast<std::size_t>(n);
  }
  if (::fsync(fd) != 0 || ::close(fd) != 0) {
    ::unlink(tmp.c_str());
    throw std::runtime_error("cannot persist state file");
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    throw std::runtime_error("cannot publish state file");
  }
}

int64_t unix_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

uint64_t monotonic_us() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

bool safe_mapfile_text(std::string_view s) {
  if (s.empty() || s.size() > 1024) return false;
  for (unsigned char c : s)
    if (c < 0x20 || c == '"' || c == '\'' || c == '\\' || c == 0x7f) return false;
  return true;
}

bool valid_identifier(std::string_view s) {
  if (s.empty() || s.size() > 63) return false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              (i > 0 && (c == '_' || c == '-'));
    if (!ok) return false;
  }
  return true;
}

const json::object &as_object(const json::value &v, std::string_view what) {
  if (!v.is_object()) throw std::runtime_error(std::string(what) + " must be an object");
  return v.get_object();
}

std::string get_string(const json::object &o, std::string_view key) {
  auto it = o.find(key);
  if (it == o.end() || !it->value().is_string())
    throw std::runtime_error("missing string field " + std::string(key));
  return std::string(it->value().get_string());
}

std::optional<std::string> opt_string(const json::object &o, std::string_view key) {
  auto it = o.find(key);
  if (it == o.end() || it->value().is_null()) return std::nullopt;
  if (!it->value().is_string()) throw std::runtime_error("field " + std::string(key) + " must be a string");
  return std::string(it->value().get_string());
}

int64_t get_int(const json::object &o, std::string_view key, int64_t lo, int64_t hi,
                std::optional<int64_t> fallback) {
  auto it = o.find(key);
  if (it == o.end()) {
    if (fallback) return *fallback;
    throw std::runtime_error("missing integer field " + std::string(key));
  }
  if (!it->value().is_int64() && !it->value().is_uint64())
    throw std::runtime_error("field " + std::string(key) + " must be an integer");
  int64_t v = it->value().is_int64() ? it->value().get_int64()
                                     : static_cast<int64_t>(it->value().get_uint64());
  if (v < lo || v > hi) throw std::runtime_error("field " + std::string(key) + " out of range");
  return v;
}

double get_number(const json::value &v) {
  if (v.is_double()) return v.get_double();
  if (v.is_int64()) return static_cast<double>(v.get_int64());
  if (v.is_uint64()) return static_cast<double>(v.get_uint64());
  throw std::runtime_error("expected a number");
}

void reject_unknown(const json::object &o, std::initializer_list<std::string_view> keys,
                    std::string_view what) {
  for (auto &kv : o) {
    bool known = false;
    for (auto k : keys) known |= kv.key() == k;
    if (!known)
      throw std::runtime_error("unknown field " + std::string(kv.key()) + " in " + std::string(what));
  }
}

void log_event(const json::object &fields) {
  static std::mutex m;
  json::object line = fields;
  line["ts_ms"] = unix_ms();
  std::string text = json::serialize(line);
  std::lock_guard lock(m);
  std::fprintf(stderr, "%s\n", text.c_str());
  std::fflush(stderr);
}

}  // namespace fvmap
