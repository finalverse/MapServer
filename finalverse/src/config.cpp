#include "config.hpp"

#include <cmath>
#include <set>

namespace fvmap {

const char *layer_kind_name(LayerKind k) {
  switch (k) {
    case LayerKind::Polygon: return "polygon";
    case LayerKind::Line: return "line";
    case LayerKind::Point: return "point";
    case LayerKind::Raster: return "raster";
  }
  return "unknown";
}

namespace {

fs::path absolute_path(const json::object &o, std::string_view key) {
  fs::path p = get_string(o, key);
  if (!p.is_absolute() || !safe_mapfile_text(p.string()))
    throw std::runtime_error(std::string(key) + " must be a plain absolute path");
  return p.lexically_normal();
}

std::array<int, 3> rgb(const json::value &v) {
  if (!v.is_array() || v.get_array().size() != 3) throw std::runtime_error("colour must be [r,g,b]");
  std::array<int, 3> out{};
  for (int i = 0; i < 3; ++i) {
    double c = get_number(v.get_array()[i]);
    if (c < 0 || c > 255 || c != std::floor(c)) throw std::runtime_error("colour component out of range");
    out[i] = static_cast<int>(c);
  }
  return out;
}

bool valid_crs(std::string_view s) {
  if (!s.starts_with("EPSG:") || s.size() < 6 || s.size() > 11) return false;
  for (char c : s.substr(5))
    if (c < '0' || c > '9') return false;
  return true;
}

LayerConfig parse_layer(const json::value &v) {
  const auto &o = as_object(v, "layer");
  reject_unknown(o, {"name", "title", "type", "source", "source_layer", "crs", "color", "outline", "width"}, "layer");
  LayerConfig l;
  l.name = get_string(o, "name");
  if (!valid_identifier(l.name)) throw std::runtime_error("invalid layer name");
  l.title = opt_string(o, "title").value_or(l.name);
  if (!safe_mapfile_text(l.title)) throw std::runtime_error("unsafe layer title");
  std::string type = get_string(o, "type");
  if (type == "polygon") l.kind = LayerKind::Polygon;
  else if (type == "line") l.kind = LayerKind::Line;
  else if (type == "point") l.kind = LayerKind::Point;
  else if (type == "raster") l.kind = LayerKind::Raster;
  else throw std::runtime_error("unknown layer type");
  l.source = absolute_path(o, "source");
  l.source_layer = opt_string(o, "source_layer").value_or("");
  if (l.kind != LayerKind::Raster && !valid_identifier(l.source_layer) &&
      !safe_mapfile_text(l.source_layer))
    throw std::runtime_error("vector layers need a safe source_layer");
  l.crs = get_string(o, "crs");
  if (!valid_crs(l.crs)) throw std::runtime_error("layer crs must be EPSG:<code>");
  if (auto it = o.find("color"); it != o.end()) l.color = rgb(it->value());
  if (auto it = o.find("outline"); it != o.end()) l.outline = rgb(it->value());
  if (auto it = o.find("width"); it != o.end()) {
    l.width = get_number(it->value());
    if (!(l.width > 0 && l.width <= 32)) throw std::runtime_error("width out of range");
  }
  return l;
}

DatasetConfig parse_dataset(const json::value &v) {
  const auto &o = as_object(v, "dataset");
  reject_unknown(o, {"id", "title", "crs", "extent", "allowed_crs", "attribution", "layers", "registration", "publication"}, "dataset");
  DatasetConfig d;
  d.id = get_string(o, "id");
  if (!valid_identifier(d.id)) throw std::runtime_error("invalid dataset id");
  d.title = opt_string(o, "title").value_or(d.id);
  d.attribution = opt_string(o, "attribution").value_or("");
  if (!safe_mapfile_text(d.title) || (!d.attribution.empty() && !safe_mapfile_text(d.attribution)))
    throw std::runtime_error("unsafe dataset title/attribution");
  d.crs = get_string(o, "crs");
  if (!valid_crs(d.crs)) throw std::runtime_error("dataset crs must be EPSG:<code>");
  const auto &ext = o.at("extent");
  if (!ext.is_array() || ext.get_array().size() != 4) throw std::runtime_error("extent must have 4 numbers");
  for (int i = 0; i < 4; ++i) {
    d.extent[i] = get_number(ext.get_array()[i]);
    if (!std::isfinite(d.extent[i])) throw std::runtime_error("extent must be finite");
  }
  if (d.extent[0] >= d.extent[2] || d.extent[1] >= d.extent[3]) throw std::runtime_error("empty extent");
  std::set<std::string> seen;
  for (auto &c : o.at("allowed_crs").as_array()) {
    std::string s(c.as_string());
    if (!valid_crs(s) || !seen.insert(s).second) throw std::runtime_error("invalid allowed_crs entry");
    d.allowed_crs.push_back(s);
  }
  if (!seen.contains(d.crs)) throw std::runtime_error("allowed_crs must contain the dataset crs");
  std::set<std::string> names;
  for (auto &l : o.at("layers").as_array()) {
    d.layers.push_back(parse_layer(l));
    if (!names.insert(d.layers.back().name).second) throw std::runtime_error("duplicate layer name");
  }
  if (d.layers.empty() || d.layers.size() > 64) throw std::runtime_error("dataset needs 1..64 layers");
  if (o.contains("registration")) d.registration = absolute_path(o, "registration");
  if (o.contains("publication")) d.publication = absolute_path(o, "publication");
  return d;
}

}  // namespace

Config parse_config(const json::value &root) {
  const auto &o = as_object(root, "configuration");
  reject_unknown(o, {"schema", "listen", "state_dir", "public_base_url", "backend", "limits", "cache", "tokens", "datasets", "worlds"}, "configuration");
  if (get_string(o, "schema") != "finalverse.map-server-config/1")
    throw std::runtime_error("unsupported configuration schema");
  Config c;
  if (auto it = o.find("listen"); it != o.end()) {
    const auto &l = as_object(it->value(), "listen");
    reject_unknown(l, {"host", "port", "allow_non_loopback"}, "listen");
    c.host = opt_string(l, "host").value_or("127.0.0.1");
    c.port = static_cast<int>(get_int(l, "port", 0, 65535, 0));
    if (auto a = l.find("allow_non_loopback"); a != l.end()) c.allow_non_loopback = a->value().as_bool();
  }
  if (!c.allow_non_loopback && c.host != "127.0.0.1" && c.host != "::1")
    throw std::runtime_error("non-loopback listening requires allow_non_loopback");
  c.state_dir = absolute_path(o, "state_dir");
  c.public_base_url = opt_string(o, "public_base_url");
  if (c.public_base_url && (!safe_mapfile_text(*c.public_base_url) ||
                            !(c.public_base_url->starts_with("http://") || c.public_base_url->starts_with("https://"))))
    throw std::runtime_error("public_base_url must be an http(s) URL");
  {
    const auto &b = as_object(o.at("backend"), "backend");
    reject_unknown(b, {"mapserv", "sha256", "env", "timeout_ms", "max_concurrency", "max_output_bytes"}, "backend");
    c.backend.mapserv = absolute_path(b, "mapserv");
    c.backend.sha256 = opt_string(b, "sha256");
    if (auto e = b.find("env"); e != b.end()) {
      for (auto &kv : e->value().as_object()) {
        std::string k(kv.key());
        static const std::set<std::string> allowed{"PROJ_DATA", "PROJ_LIB", "GDAL_DATA", "DYLD_LIBRARY_PATH", "LD_LIBRARY_PATH", "MS_ERRORFILE", "CPL_DEBUG"};
        if (!allowed.contains(k)) throw std::runtime_error("backend env " + k + " is not on the allow list");
        std::string val(kv.value().as_string());
        if (!safe_mapfile_text(val)) throw std::runtime_error("unsafe backend env value");
        c.backend.env[k] = val;
      }
    }
    c.backend.timeout_ms = static_cast<int>(get_int(b, "timeout_ms", 100, 120000, 10000));
    c.backend.max_concurrency = static_cast<int>(get_int(b, "max_concurrency", 1, 64, 4));
    c.backend.max_output_bytes = static_cast<std::size_t>(get_int(b, "max_output_bytes", 4096, 64 << 20, 8 << 20));
  }
  if (auto it = o.find("limits"); it != o.end()) {
    const auto &l = as_object(it->value(), "limits");
    reject_unknown(l, {"worker_threads", "max_request_bytes", "max_width", "max_height", "io_timeout_ms", "queue_timeout_ms", "rate_per_second", "burst", "max_keepalive_requests"}, "limits");
    c.limits.worker_threads = static_cast<int>(get_int(l, "worker_threads", 1, 256, 8));
    c.limits.max_request_bytes = static_cast<std::size_t>(get_int(l, "max_request_bytes", 1024, 65536, 8192));
    c.limits.max_width = static_cast<int>(get_int(l, "max_width", 16, 8192, 2048));
    c.limits.max_height = static_cast<int>(get_int(l, "max_height", 16, 8192, 2048));
    c.limits.io_timeout_ms = static_cast<int>(get_int(l, "io_timeout_ms", 100, 60000, 5000));
    c.limits.queue_timeout_ms = static_cast<int>(get_int(l, "queue_timeout_ms", 0, 60000, 2000));
    c.limits.rate_per_second = static_cast<double>(get_int(l, "rate_per_second", 1, 100000, 50));
    c.limits.burst = static_cast<double>(get_int(l, "burst", 1, 100000, 100));
    c.limits.max_keepalive_requests = static_cast<int>(get_int(l, "max_keepalive_requests", 1, 10000, 100));
  }
  if (auto it = o.find("cache"); it != o.end()) {
    const auto &l = as_object(it->value(), "cache");
    reject_unknown(l, {"max_bytes", "max_entries"}, "cache");
    c.cache.max_bytes = static_cast<std::size_t>(get_int(l, "max_bytes", 0, int64_t(1) << 40, 64 << 20));
    c.cache.max_entries = static_cast<std::size_t>(get_int(l, "max_entries", 0, 1 << 24, 4096));
  }
  c.tokens = absolute_path(o, "tokens");
  std::set<std::string> ids;
  for (auto &d : o.at("datasets").as_array()) {
    c.datasets.push_back(parse_dataset(d));
    if (!ids.insert(c.datasets.back().id).second) throw std::runtime_error("duplicate dataset id");
  }
  if (auto it = o.find("worlds"); it != o.end()) {
    std::set<std::string> wids;
    for (auto &wv : it->value().as_array()) {
      const auto &w = as_object(wv, "world");
      reject_unknown(w, {"id", "title", "dataset", "store", "registration"}, "world");
      WorldConfig wc;
      wc.id = get_string(w, "id");
      if (!valid_identifier(wc.id) || !wids.insert(wc.id).second) throw std::runtime_error("invalid or duplicate world id");
      wc.title = opt_string(w, "title").value_or(wc.id);
      wc.dataset = opt_string(w, "dataset").value_or("");
      if (!wc.dataset.empty() && !ids.contains(wc.dataset)) throw std::runtime_error("world references an unknown dataset");
      wc.store = absolute_path(w, "store");
      if (w.contains("registration")) wc.registration = absolute_path(w, "registration");
      c.worlds.push_back(std::move(wc));
    }
  }
  return c;
}

Config load_config(const fs::path &path) {
  std::string text = read_file(path, 1 << 20);
  boost::system::error_code ec;
  json::value v = json::parse(text, ec);
  if (ec) throw std::runtime_error("configuration is not valid JSON");
  return parse_config(v);
}

}  // namespace fvmap
