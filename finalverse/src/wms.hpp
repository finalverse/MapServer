// Strict WMS request validation and canonicalisation. WMS itself is executed
// by MapServer; this layer only decides what may reach it.
#pragma once
#include <array>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "catalog.hpp"

namespace fvmap {

struct WmsRequest {
  std::string version;  // 1.1.1 or 1.3.0
  std::string request;  // GetMap or GetCapabilities
  std::vector<std::pair<std::string, std::string>> canonical;  // sorted, upper-case keys
  std::string canonical_query() const;  // URL-encoded, deterministic
  bool is_map() const { return request == "GetMap"; }
  std::string format;
  int width = 0, height = 0;
};

// Transformed dataset extents per CRS, computed once with PROJ via GDAL.
class ExtentIndex {
 public:
  // Returns [minx,miny,maxx,maxy] in the CRS's traditional GIS (east,north) order.
  std::array<double, 4> extent_in(const DatasetState &d, const std::string &crs);

 private:
  std::mutex m_;
  std::map<std::string, std::array<double, 4>> cache_;
};

// True when WMS 1.3.0 expects (north,east) axis order for this CRS.
bool crs_is_northing_first(const std::string &crs);

// Throws Error(400/404) with a stable code when the request is not admissible.
WmsRequest validate_wms(std::string_view raw_query, const DatasetState &d, const Limits &limits,
                        ExtentIndex &extents);

}  // namespace fvmap
