// World Scene API: read-only views of MutSea region stores through the MutSea
// native projection library. The Map Server never writes world state.
#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "config.hpp"

namespace fvmap {

class WorldService {
 public:
  explicit WorldService(const Config &config);
  ~WorldService();
  bool enabled() const;
  json::array list();
  json::object head(const std::string &id);
  json::object snapshot(const std::string &id);
  // WorldLine events strictly after `after_seq`; `after_hash` (optional) must
  // match the chain hash at after_seq or the client is told to resynchronise.
  json::object events(const std::string &id, uint64_t after_seq, const std::string &after_hash, uint32_t limit);
  // Same events as concatenated FVP frames (application/x-finalverse-fvp).
  std::string events_fvp(const std::string &id, uint64_t after_seq, const std::string &after_hash, uint32_t limit);
  json::object registration(const std::string &id);
  const WorldConfig *find(const std::string &id) const;

 private:
  struct World;
  [[maybe_unused]] const Config &config_;
  std::vector<std::unique_ptr<World>> worlds_;
  World &get(const std::string &id);
};

}  // namespace fvmap
