// libFuzzer target: arbitrary query strings through WMS validation.
#include <gdal_priv.h>

#include "wms.hpp"

using namespace fvmap;

namespace {
struct Fixture {
  Config config;
  DatasetState state;
  ExtentIndex extents;
  Fixture() {
    GDALAllRegister();
    config = parse_config(json::parse(R"({"schema":"finalverse.map-server-config/1","state_dir":"/tmp/fvmap-fuzz",
      "tokens":"/tmp/fvmap-fuzz/t.json","backend":{"mapserv":"/usr/bin/true"},
      "datasets":[{"id":"montreal","crs":"EPSG:2950","extent":[300489,5040679,300749,5040939],
        "allowed_crs":["EPSG:2950","EPSG:4326","EPSG:3857"],
        "layers":[{"name":"roofs","type":"polygon","source":"/x/b.geojson","source_layer":"buildings","crs":"EPSG:2950"},
                  {"name":"roads","type":"line","source":"/x/r.geojson","source_layer":"roads","crs":"EPSG:4326"}]}]})"));
    state.config = &config.datasets[0];
    state.version = std::string(64, 'a');
  }
};
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  static Fixture f;
  std::string_view q(reinterpret_cast<const char *>(data), size);
  try {
    WmsRequest r = validate_wms(q, f.state, f.config.limits, f.extents);
    std::string c = r.canonical_query();
    // Canonical output must itself validate to the same canonical form.
    WmsRequest again = validate_wms(c, f.state, f.config.limits, f.extents);
    if (again.canonical_query() != c) __builtin_trap();
    if (c.find("MAP=") != std::string::npos || c.find("map=") != std::string::npos) __builtin_trap();
  } catch (const Error &) {
  }
  return 0;
}
