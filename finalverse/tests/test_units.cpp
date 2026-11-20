// Unit tests: configuration, tokens, WMS validation, cache recovery.
#include <cassert>
#include <cstdio>
#include <cstdlib>

#include <gdal_priv.h>

#include "auth.hpp"
#include "cache.hpp"
#include "config.hpp"
#include "wms.hpp"

using namespace fvmap;

static int failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                          \
    }                                                                      \
  } while (0)

template <class F>
static std::string error_code(F f) {
  try {
    f();
  } catch (const Error &e) {
    return e.code;
  } catch (const std::exception &e) {
    return std::string("std:") + e.what();
  }
  return "none";
}

static json::value base_config(const fs::path &dir) {
  return json::parse(R"({
    "schema":"finalverse.map-server-config/1",
    "state_dir":")" + (dir / "state").string() + R"(",
    "tokens":")" + (dir / "tokens.json").string() + R"(",
    "backend":{"mapserv":"/usr/bin/true"},
    "datasets":[{"id":"montreal","crs":"EPSG:2950","extent":[300489,5040679,300749,5040939],
      "allowed_crs":["EPSG:2950","EPSG:4326","EPSG:3857"],
      "layers":[{"name":"roofs","type":"polygon","source":"/x/buildings.geojson","source_layer":"buildings","crs":"EPSG:2950"},
                {"name":"roads","type":"line","source":"/x/roads.geojson","source_layer":"roads","crs":"EPSG:4326"}]}]})");
}

int main(int, char **) {
  GDALAllRegister();
  fs::path dir = fs::temp_directory_path() / ("fvmap-unit-" + random_hex(6));
  fs::create_directories(dir);

  // Configuration validation.
  Config c = parse_config(base_config(dir));
  CHECK(c.datasets.size() == 1 && c.datasets[0].layers.size() == 2);
  CHECK(c.host == "127.0.0.1");
  {
    auto v = base_config(dir);
    v.as_object()["listen"] = json::object{{"host", "0.0.0.0"}};
    CHECK(error_code([&] { parse_config(v); }).starts_with("std:non-loopback"));
    v = base_config(dir);
    v.as_object()["datasets"].as_array()[0].as_object()["layers"].as_array()[0].as_object()["source"] = "/x/a\"b";
    CHECK(error_code([&] { parse_config(v); }) != "none");
    v = base_config(dir);
    v.as_object()["surprise"] = 1;
    CHECK(error_code([&] { parse_config(v); }).find("unknown field") != std::string::npos);
    v = base_config(dir);
    v.as_object()["backend"].as_object()["env"] = json::object{{"LD_PRELOAD", "/evil"}};
    CHECK(error_code([&] { parse_config(v); }).find("allow list") != std::string::npos);
  }

  // Tokens: digests only, scopes, constant-time matching.
  std::string token = random_hex(32);
  auto store = TokenStore::from_json(json::parse(R"({"schema":"finalverse.map-server-tokens/1","principals":[
    {"principal":"viewer","token_sha256":")" + sha256_hex(token) + R"(","role":"reader","datasets":["montreal"]}]})"));
  auto p = store.authenticate("Bearer " + token);
  CHECK(p && p->name == "viewer" && p->may_read_dataset("montreal") && !p->may_read_dataset("other"));
  CHECK(!store.authenticate("Bearer " + token + "x"));
  CHECK(!store.authenticate(token));
  CHECK(!store.authenticate("Bearer short"));
  RateLimiter rl(1, 2);
  CHECK(rl.allow("a") && rl.allow("a") && !rl.allow("a") && rl.allow("b"));

  // WMS validation against a dataset state.
  DatasetState d;
  d.config = &c.datasets[0];
  d.version = std::string(64, 'a');
  ExtentIndex ext;
  Limits lim;
  auto ok = validate_wms("SERVICE=WMS&VERSION=1.1.1&REQUEST=GetMap&LAYERS=roofs,roads&STYLES=&SRS=EPSG:2950"
                         "&BBOX=300489,5040679,300749,5040939&WIDTH=256&HEIGHT=256&FORMAT=image/png",
                         d, lim, ext);
  CHECK(ok.is_map() && ok.width == 256);
  // Case-insensitive keys and parameter order produce the same canonical query.
  auto ok2 = validate_wms("format=image/png&height=256&width=256&bbox=300489.0,5040679,300749,5040939&srs=epsg:2950"
                          "&styles=&layers=roofs,roads&request=GetMap&version=1.1.1&service=WMS",
                          d, lim, ext);
  CHECK(ok.canonical_query() == ok2.canonical_query());
  auto bad = [&](const char *q) {
    return error_code([&] { validate_wms(q, d, lim, ext); });
  };
  const char *good_tail = "&BBOX=300489,5040679,300749,5040939&WIDTH=256&HEIGHT=256&FORMAT=image/png";
  std::string base = "SERVICE=WMS&VERSION=1.1.1&REQUEST=GetMap&LAYERS=roofs&SRS=EPSG:2950";
  CHECK(bad((base + good_tail + "&MAP=/etc/passwd").c_str()) == "unsupported_parameter");
  CHECK(bad((base + good_tail + "&WIDTH=10").c_str()) == "duplicate_parameter");
  CHECK(bad(("SERVICE=WMS&VERSION=1.1.1&REQUEST=GetMap&LAYERS=secret&SRS=EPSG:2950" + std::string(good_tail)).c_str()) == "unknown_layer");
  CHECK(bad(("SERVICE=WMS&VERSION=1.1.1&REQUEST=GetMap&LAYERS=roofs&SRS=EPSG:32618" + std::string(good_tail)).c_str()) == "unsupported_crs");
  CHECK(bad((base + "&BBOX=300489,5040679,300749,5040939&WIDTH=99999&HEIGHT=256&FORMAT=image/png").c_str()) == "invalid_size");
  CHECK(bad((base + "&BBOX=1,2,3&WIDTH=256&HEIGHT=256&FORMAT=image/png").c_str()) == "invalid_bbox");
  CHECK(bad((base + "&BBOX=nan,2,3,4&WIDTH=256&HEIGHT=256&FORMAT=image/png").c_str()) == "invalid_bbox");
  CHECK(bad((base + "&BBOX=0,0,10,10&WIDTH=256&HEIGHT=256&FORMAT=image/png").c_str()) == "outside_extent");
  CHECK(bad((base + "&BBOX=300489,5040679,300749,5040939&WIDTH=256&HEIGHT=256&FORMAT=image/tiff").c_str()) == "unsupported_format");
  CHECK(bad((base + good_tail + "&STYLES=evil").c_str()) == "unsupported_style");
  CHECK(bad("SERVICE=WMS&REQUEST=GetCapabilities&LAYERS=roofs") == "unsupported_parameter");
  CHECK(bad("SERVICE=WFS&REQUEST=GetCapabilities") == "unsupported_service");
  CHECK(bad((base + good_tail + "&TRANSPARENT=%0Atrue").c_str()) == "malformed_query");
  // WMS 1.3.0 with EPSG:4326 uses latitude,longitude order.
  CHECK(error_code([&] {
          validate_wms("SERVICE=WMS&VERSION=1.3.0&REQUEST=GetMap&LAYERS=roofs&STYLES=&CRS=EPSG:4326"
                       "&BBOX=45.50,-73.56,45.51,-73.55&WIDTH=256&HEIGHT=256&FORMAT=image/png",
                       d, lim, ext);
        }) == "none");
  CHECK(bad("SERVICE=WMS&VERSION=1.3.0&REQUEST=GetMap&LAYERS=roofs&STYLES=&CRS=EPSG:4326"
            "&BBOX=-73.56,45.50,-73.55,45.51&WIDTH=256&HEIGHT=256&FORMAT=image/png") == "outside_extent");
  CHECK(bad("SERVICE=WMS&VERSION=1.3.0&REQUEST=GetMap&LAYERS=roofs&STYLES=&SRS=EPSG:2950"
            "&BBOX=300489,5040679,300749,5040939&WIDTH=256&HEIGHT=256&FORMAT=image/png") == "unsupported_parameter");
  CHECK(bad((base + "&BBOX=-1e9,-1e9,1e9,1e9&WIDTH=256&HEIGHT=256&FORMAT=image/png").c_str()) == "bbox_too_large");

  // Cache: keys bind version and backend; entries survive restart; corruption is never served.
  std::string k1 = ResponseCache::make_key("montreal", "v1", "b", "q");
  CHECK(k1 != ResponseCache::make_key("montreal", "v2", "b", "q"));
  CHECK(k1 != ResponseCache::make_key("montreal", "v1", "c", "q"));
  std::string k2 = ResponseCache::make_key("montreal", "v1", "b", "q2");
  {
    ResponseCache cache(dir / "cache", CacheConfig{1 << 20, 16});
    cache.put(k1, "montreal", "v1", "image/png", "PNGDATA1");
    cache.put(k2, "montreal", "v1", "image/png", "PNGDATA2");
    CHECK(cache.get(k1)->body == "PNGDATA1");
  }
  {
    ResponseCache cache(dir / "cache", CacheConfig{1 << 20, 16});
    CHECK(cache.recovered() == 2);
    CHECK(cache.get(k1) && cache.get(k1)->body == "PNGDATA1");
    // Tamper with one body on disk.
    fs::path body = dir / "cache" / k2.substr(0, 2) / (k2 + ".body");
    write_file_atomic(body, "PNGDATAX");
    CHECK(!cache.get(k2));
    CHECK(cache.entries() == 1);
    CHECK(cache.invalidate({{"montreal", "v2"}}) == 1);
    CHECK(cache.entries() == 0);
  }
  {
    ResponseCache cache(dir / "cache", CacheConfig{20, 16});  // tiny byte budget
    for (int i = 0; i < 10; ++i)
      cache.put(ResponseCache::make_key("m", "v", "b", std::to_string(i)), "m", "v", "x", "12345");
    CHECK(cache.bytes() <= 20);
  }
  fs::remove_all(dir);
  if (failures) {
    std::fprintf(stderr, "%d unit check(s) failed\n", failures);
    return 1;
  }
  std::printf("fvmap unit tests passed\n");
  return 0;
}
