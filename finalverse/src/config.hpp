// Finalverse Map Server configuration (finalverse.map-server-config/1).
#pragma once
#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "util.hpp"

namespace fvmap {

enum class LayerKind { Polygon, Line, Point, Raster };
const char *layer_kind_name(LayerKind);

struct LayerConfig {
  std::string name, title;
  LayerKind kind = LayerKind::Polygon;
  fs::path source;
  std::string source_layer;  // OGR layer name; empty for rasters
  std::string crs;           // authoritative CRS declared by the operator
  std::array<int, 3> color{128, 128, 128};
  std::optional<std::array<int, 3>> outline;
  double width = 1.0;
};

struct DatasetConfig {
  std::string id, title, crs, attribution;
  std::array<double, 4> extent{};  // in crs
  std::vector<std::string> allowed_crs;
  std::vector<LayerConfig> layers;
  std::optional<fs::path> registration;  // finalverse.geographic-registration document
  std::optional<fs::path> publication;   // persisted publication reference
};

struct WorldConfig {
  std::string id, title, dataset;
  fs::path store;  // MutSea region store, opened read-only
  std::optional<fs::path> registration;
};

struct BackendConfig {
  fs::path mapserv;
  std::optional<std::string> sha256;  // pinned executable digest
  std::map<std::string, std::string> env;
  int timeout_ms = 10000;
  int max_concurrency = 4;
  std::size_t max_output_bytes = 8u << 20;
};

struct Limits {
  int worker_threads = 8;
  std::size_t max_request_bytes = 8192;
  int max_width = 2048, max_height = 2048;
  int io_timeout_ms = 5000;
  int queue_timeout_ms = 2000;
  double rate_per_second = 50, burst = 100;
  int max_keepalive_requests = 100;
};

struct CacheConfig {
  std::size_t max_bytes = 64u << 20;
  std::size_t max_entries = 4096;
};

struct Config {
  std::string host = "127.0.0.1";
  int port = 0;
  bool allow_non_loopback = false;
  fs::path state_dir;
  std::optional<std::string> public_base_url;
  BackendConfig backend;
  Limits limits;
  CacheConfig cache;
  fs::path tokens;
  std::vector<DatasetConfig> datasets;
  std::vector<WorldConfig> worlds;
};

Config load_config(const fs::path &path);
Config parse_config(const json::value &root);

}  // namespace fvmap
