#include "wms.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <memory>
#include <set>

#include <boost/url.hpp>
#include <ogr_spatialref.h>

namespace fvmap {

namespace {

std::string upper(std::string_view s) {
  std::string out(s);
  for (char &c : out)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
  return out;
}

int parse_dimension(const std::string &s, int max) {
  int v = 0;
  auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || p != s.data() + s.size() || v < 1 || v > max)
    throw Error(400, "invalid_size", "WIDTH and HEIGHT must be integers within the service limits");
  return v;
}

std::string fmt(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.17g", v == 0.0 ? 0.0 : v);
  return buf;
}

std::array<double, 4> parse_bbox(const std::string &s) {
  std::array<double, 4> out{};
  std::size_t start = 0;
  for (int i = 0; i < 4; ++i) {
    std::size_t end = s.find(',', start);
    if ((i < 3) != (end != std::string::npos)) throw Error(400, "invalid_bbox", "BBOX must have four values");
    std::string part = s.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (part.empty() || part.size() > 40) throw Error(400, "invalid_bbox", "BBOX value is malformed");
    char *stop = nullptr;
    out[i] = std::strtod(part.c_str(), &stop);
    if (stop != part.c_str() + part.size() || !std::isfinite(out[i]))
      throw Error(400, "invalid_bbox", "BBOX values must be finite numbers");
    start = end + 1;
  }
  if (out[0] >= out[2] || out[1] >= out[3]) throw Error(400, "invalid_bbox", "BBOX must be ordered min,max");
  return out;
}

std::unique_ptr<OGRSpatialReference> srs_for(const std::string &crs) {
  auto srs = std::make_unique<OGRSpatialReference>();
  if (srs->SetFromUserInput(crs.c_str()) != OGRERR_NONE) throw Error(400, "unsupported_crs", "CRS is unknown");
  return srs;
}

}  // namespace

bool crs_is_northing_first(const std::string &crs) {
  auto srs = srs_for(crs);
  return srs->EPSGTreatsAsLatLong() || srs->EPSGTreatsAsNorthingEasting();
}

std::array<double, 4> ExtentIndex::extent_in(const DatasetState &d, const std::string &crs) {
  std::string key = d.version + "|" + crs;
  std::lock_guard lock(m_);
  if (auto it = cache_.find(key); it != cache_.end()) return it->second;
  const auto &e = d.config->extent;
  std::array<double, 4> out = e;
  if (crs != d.config->crs) {
    auto src = srs_for(d.config->crs), dst = srs_for(crs);
    src->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    dst->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    std::unique_ptr<OGRCoordinateTransformation> ct(OGRCreateCoordinateTransformation(src.get(), dst.get()));
    if (!ct) throw Error(400, "unsupported_crs", "no coordinate operation to the requested CRS");
    double xmin, ymin, xmax, ymax;
    if (!ct->TransformBounds(e[0], e[1], e[2], e[3], &xmin, &ymin, &xmax, &ymax, 21))
      throw Error(400, "unsupported_crs", "dataset extent cannot be expressed in the requested CRS");
    out = {xmin, ymin, xmax, ymax};
  }
  cache_.emplace(key, out);
  return out;
}

std::string WmsRequest::canonical_query() const {
  boost::urls::url u;
  for (auto &[k, v] : canonical) u.params().append({k, v});
  return std::string(u.encoded_query());
}

WmsRequest validate_wms(std::string_view raw_query, const DatasetState &d, const Limits &limits,
                        ExtentIndex &extents) {
  if (raw_query.size() > 2048) throw Error(414, "query_too_long", "query string exceeds its bound");
  const std::string query_text(raw_query);  // owned, NUL-terminated
  auto parsed = boost::urls::parse_query(query_text);
  if (!parsed) throw Error(400, "malformed_query", "query string is not well formed");
  static const std::set<std::string> allowed{"SERVICE", "VERSION", "REQUEST", "LAYERS", "STYLES", "SRS", "CRS",
                                             "BBOX", "WIDTH", "HEIGHT", "FORMAT", "TRANSPARENT", "BGCOLOR"};
  std::map<std::string, std::string> q;
  for (auto p : *parsed) {
    std::string key = upper(p.key.decode());
    std::string value = p.value.decode();
    if (!allowed.contains(key)) throw Error(400, "unsupported_parameter", "parameter is not accepted: " + key);
    if (!p.has_value) throw Error(400, "malformed_query", "parameter without value: " + key);
    if (value.size() > 512) throw Error(400, "malformed_query", "parameter value too long");
    for (unsigned char c : value)
      if (c < 0x20 || c == 0x7f) throw Error(400, "malformed_query", "control characters are not accepted");
    if (!q.emplace(key, value).second)
      throw Error(400, "duplicate_parameter", "parameter repeated: " + key);
  }
  auto need = [&](const char *k) -> const std::string & {
    auto it = q.find(k);
    if (it == q.end()) throw Error(400, "missing_parameter", std::string("missing parameter ") + k);
    return it->second;
  };
  if (need("SERVICE") != "WMS") throw Error(400, "unsupported_service", "SERVICE must be WMS");
  WmsRequest r;
  r.request = need("REQUEST");
  r.version = q.contains("VERSION") ? q["VERSION"] : (r.request == "GetCapabilities" ? "1.3.0" : "");
  if (r.version != "1.1.1" && r.version != "1.3.0")
    throw Error(400, "unsupported_version", "VERSION must be 1.1.1 or 1.3.0");
  if (r.request == "GetCapabilities") {
    for (auto &[k, v] : q)
      if (k != "SERVICE" && k != "VERSION" && k != "REQUEST")
        throw Error(400, "unsupported_parameter", "GetCapabilities takes no map parameters");
    r.canonical = {{"REQUEST", r.request}, {"SERVICE", "WMS"}, {"VERSION", r.version}};
    return r;
  }
  if (r.request != "GetMap") throw Error(400, "unsupported_request", "REQUEST must be GetMap or GetCapabilities");
  const char *crs_key = r.version == "1.3.0" ? "CRS" : "SRS";
  if (q.contains(r.version == "1.3.0" ? "SRS" : "CRS"))
    throw Error(400, "unsupported_parameter", "use SRS with 1.1.1 and CRS with 1.3.0");
  std::string crs = upper(need(crs_key));
  if (std::find(d.config->allowed_crs.begin(), d.config->allowed_crs.end(), crs) == d.config->allowed_crs.end())
    throw Error(400, "unsupported_crs", "CRS is not offered by this dataset");
  // Layers must exist, be unique and keep their requested drawing order.
  std::vector<std::string> layers;
  std::set<std::string> seen;
  const std::string &layer_list = need("LAYERS");
  std::size_t start = 0;
  while (start <= layer_list.size()) {
    std::size_t end = layer_list.find(',', start);
    std::string name = layer_list.substr(start, end == std::string::npos ? std::string::npos : end - start);
    bool known = false;
    for (auto &l : d.config->layers) known |= l.name == name;
    if (!known) throw Error(404, "unknown_layer", "layer is not in this dataset");
    if (!seen.insert(name).second) throw Error(400, "duplicate_layer", "layer repeated");
    layers.push_back(name);
    if (end == std::string::npos) break;
    start = end + 1;
  }
  if (auto it = q.find("STYLES"); it != q.end() && !it->second.empty() &&
      it->second != std::string(layers.size() - 1, ','))
    throw Error(400, "unsupported_style", "only default styles are offered");
  r.width = parse_dimension(need("WIDTH"), limits.max_width);
  r.height = parse_dimension(need("HEIGHT"), limits.max_height);
  r.format = need("FORMAT");
  if (r.format != "image/png" && r.format != "image/jpeg")
    throw Error(400, "unsupported_format", "FORMAT must be image/png or image/jpeg");
  std::string transparent = q.contains("TRANSPARENT") ? upper(q["TRANSPARENT"]) : "FALSE";
  if (transparent != "TRUE" && transparent != "FALSE")
    throw Error(400, "invalid_parameter", "TRANSPARENT must be TRUE or FALSE");
  std::string bg;
  if (auto it = q.find("BGCOLOR"); it != q.end()) {
    bg = upper(it->second);
    if (bg.size() != 8 || !bg.starts_with("0X") || bg.find_first_not_of("0123456789ABCDEF", 2) != std::string::npos)
      throw Error(400, "invalid_parameter", "BGCOLOR must be 0xRRGGBB");
  }
  auto box = parse_bbox(need("BBOX"));
  // Normalise to (east,north) to compare with the dataset footprint.
  std::array<double, 4> en = box;
  bool swapped = r.version == "1.3.0" && crs_is_northing_first(crs);
  if (swapped) en = {box[1], box[0], box[3], box[2]};
  auto ext = extents.extent_in(d, crs);
  if (en[2] < ext[0] || en[0] > ext[2] || en[3] < ext[1] || en[1] > ext[3])
    throw Error(400, "outside_extent", "BBOX does not intersect the dataset");
  double ew = ext[2] - ext[0], eh = ext[3] - ext[1];
  if (en[2] - en[0] > 16 * ew || en[3] - en[1] > 16 * eh)
    throw Error(400, "bbox_too_large", "BBOX is far larger than the dataset footprint");
  std::string list;
  for (auto &l : layers) list += (list.empty() ? "" : ",") + l;
  r.canonical = {{"BBOX", fmt(box[0]) + "," + fmt(box[1]) + "," + fmt(box[2]) + "," + fmt(box[3])},
                 {crs_key, crs},
                 {"FORMAT", r.format},
                 {"HEIGHT", std::to_string(r.height)},
                 {"LAYERS", list},
                 {"REQUEST", "GetMap"},
                 {"SERVICE", "WMS"},
                 {"STYLES", std::string(layers.size() - 1, ',')},
                 {"TRANSPARENT", transparent},
                 {"VERSION", r.version},
                 {"WIDTH", std::to_string(r.width)}};
  if (!bg.empty()) r.canonical.insert(r.canonical.begin(), {"BGCOLOR", bg});
  return r;
}

}  // namespace fvmap
