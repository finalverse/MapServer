// Versioned map catalog: source digests, dataset versions, generated mapfiles.
#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>

#include "config.hpp"

namespace fvmap {

struct SourceState {
  std::string sha256;
  uint64_t size = 0;
  int64_t mtime_ns = 0;
  uint64_t inode = 0;
};

struct DatasetState {
  const DatasetConfig *config = nullptr;
  std::string version;  // sha256 over configuration and source digests
  std::map<std::string, SourceState> sources;  // by layer name
  std::string map_alias;   // MAPS key inside the MapServer config file
  fs::path mapfile;
  json::object registration;  // summary; empty when none configured
  json::value publication;    // persisted publication reference or null
  int64_t loaded_unix_ms = 0;
};

struct CatalogState {
  std::map<std::string, DatasetState> datasets;
  fs::path mapserver_config;
  uint64_t generation = 0;
};

class Catalog {
 public:
  Catalog(const Config &config, std::string public_base_url);
  // Hashes sources, writes mapfiles and swaps in a new generation.
  std::shared_ptr<const CatalogState> reload();
  std::shared_ptr<const CatalogState> current() const;
  // True when a source no longer matches its recorded digest.
  bool stale(const DatasetState &d) const;
  json::object describe(const DatasetState &d) const;
  json::array list() const;
  const Config &config() const { return config_; }

 private:
  const Config &config_;
  std::string base_url_;
  mutable std::shared_mutex m_;
  std::shared_ptr<const CatalogState> state_;
  uint64_t generation_ = 0;
  std::string render_mapfile(const DatasetState &d) const;
};

SourceState stat_source(const fs::path &p);

}  // namespace fvmap
