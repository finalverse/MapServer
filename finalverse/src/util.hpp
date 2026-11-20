// Finalverse Map Server: shared utilities.
#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <boost/json.hpp>

namespace fvmap {

namespace fs = std::filesystem;
namespace json = boost::json;

// An error with a stable machine code and an HTTP status. Messages never carry
// secrets or absolute paths of the host.
struct Error : std::runtime_error {
  int status;
  std::string code;
  Error(int s, std::string c, const std::string &message)
      : std::runtime_error(message), status(s), code(std::move(c)) {}
};

std::string sha256_hex(std::string_view bytes);
std::string sha256_file_hex(const fs::path &path);
std::string hex(const unsigned char *data, std::size_t size);
bool constant_time_equal(std::string_view a, std::string_view b);
std::string random_hex(std::size_t bytes);

std::string read_file(const fs::path &path, std::size_t max_bytes);
// Writes through a temporary file in the same directory and renames it.
void write_file_atomic(const fs::path &path, std::string_view bytes);

int64_t unix_ms();
uint64_t monotonic_us();

// Refuses strings MapServer mapfiles or CGI cannot carry safely.
bool safe_mapfile_text(std::string_view s);
bool valid_identifier(std::string_view s);  // [a-z0-9][a-z0-9_-]{0,62}

// Strict JSON accessors used by configuration loaders.
const json::object &as_object(const json::value &v, std::string_view what);
std::string get_string(const json::object &o, std::string_view key);
std::optional<std::string> opt_string(const json::object &o, std::string_view key);
int64_t get_int(const json::object &o, std::string_view key, int64_t lo, int64_t hi,
                std::optional<int64_t> fallback = std::nullopt);
double get_number(const json::value &v);
void reject_unknown(const json::object &o, std::initializer_list<std::string_view> keys,
                    std::string_view what);

// One structured JSON log line on stderr; thread-safe.
void log_event(const json::object &fields);

}  // namespace fvmap
