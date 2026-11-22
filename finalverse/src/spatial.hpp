// Deterministic spatial operations over cataloged datasets (GDAL/OGR/GEOS).
// These back the typed AI operations spatial.inspect_region,
// spatial.query_terrain and spatial.find_nearby; results are computed, never
// generated.
#pragma once
#include <array>
#include <optional>

#include "catalog.hpp"

namespace fvmap {

struct InspectQuery {
  std::array<double, 4> bbox;  // east,north order in crs
  std::string crs;
};

struct NearbyQuery {
  double x = 0, y = 0;  // east,north in crs
  std::string crs;
  double radius_m = 50;
  std::optional<std::string> layer;
  int limit = 20;
};

json::object spatial_inspect(const DatasetState &d, const InspectQuery &q);
json::object spatial_nearby(const DatasetState &d, const NearbyQuery &q);

// Parses query strings for the two routes; throws Error(400) on bad input.
InspectQuery parse_inspect(std::string_view query, const DatasetState &d);
NearbyQuery parse_nearby(std::string_view query, const DatasetState &d);

}  // namespace fvmap
