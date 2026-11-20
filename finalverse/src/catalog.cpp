#include "catalog.hpp"

#include <sys/stat.h>

#include <sstream>

namespace fvmap {

SourceState stat_source(const fs::path &p) {
  struct stat st{};
  if (::lstat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
    throw Error(503, "source_missing", "a dataset source is missing or not a regular file");
  SourceState s;
  s.size = static_cast<uint64_t>(st.st_size);
#ifdef __APPLE__
  s.mtime_ns = int64_t(st.st_mtimespec.tv_sec) * 1000000000 + st.st_mtimespec.tv_nsec;
#else
  s.mtime_ns = int64_t(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
#endif
  s.inode = static_cast<uint64_t>(st.st_ino);
  return s;
}

Catalog::Catalog(const Config &config, std::string public_base_url)
    : config_(config), base_url_(std::move(public_base_url)) {}

namespace {

json::object summarize_registration(const fs::path &path) {
  json::object out;
  std::string bytes = read_file(path, 1 << 20);
  out["sha256"] = sha256_hex(bytes);
  boost::system::error_code ec;
  json::value v = json::parse(bytes, ec);
  if (ec || !v.is_object()) throw std::runtime_error("registration document is not a JSON object");
  const auto &o = v.get_object();
  out["schema"] = o.contains("schema") ? o.at("schema") : json::value(nullptr);
  out["revision"] = o.contains("revision") ? o.at("revision") : json::value(nullptr);
  if (auto s = o.if_contains("source"); s && s->is_object()) {
    for (const char *k : {"crs", "vertical_datum", "frame_id"})
      if (auto x = s->get_object().if_contains(k)) out[std::string("source_") + k] = *x;
  }
  if (auto val = o.if_contains("validation"); val && val->is_object()) {
    const auto &vo = val->get_object();
    for (const char *k : {"status", "surveyed", "coordinate_epoch"})
      out[k] = vo.contains(k) ? vo.at(k) : json::value(nullptr);
  }
  if (auto a = o.if_contains("authorization"); a && a->is_object())
    if (auto sc = a->get_object().if_contains("scope")) out["scope"] = *sc;
  return out;
}

}  // namespace

std::shared_ptr<const CatalogState> Catalog::reload() {
  auto next = std::make_shared<CatalogState>();
  fs::path maps = config_.state_dir / "maps";
  fs::create_directories(maps);
  fs::permissions(maps, fs::perms::owner_all, fs::perm_options::replace);
  std::ostringstream conf;
  conf << "CONFIG\n  ENV\n    MS_MAP_NO_PATH \"1\"\n";
  for (auto &[k, v] : config_.backend.env)
    if (k == "PROJ_DATA" || k == "PROJ_LIB") conf << "    " << k << " \"" << v << "\"\n";
  conf << "  END\n  MAPS\n";
  for (const auto &dc : config_.datasets) {
    DatasetState d;
    d.config = &dc;
    json::object basis;
    basis["schema"] = "finalverse.map-dataset-version/1";
    basis["id"] = dc.id;
    basis["crs"] = dc.crs;
    json::array layers;
    for (const auto &l : dc.layers) {
      SourceState s = stat_source(l.source);
      s.sha256 = sha256_file_hex(l.source);
      SourceState after = stat_source(l.source);
      if (after.mtime_ns != s.mtime_ns || after.size != s.size)
        throw Error(503, "source_changing", "a dataset source changed while it was hashed");
      json::object lo;
      lo["name"] = l.name;
      lo["type"] = layer_kind_name(l.kind);
      lo["crs"] = l.crs;
      lo["source_layer"] = l.source_layer;
      lo["sha256"] = s.sha256;
      lo["color"] = json::array{l.color[0], l.color[1], l.color[2]};
      lo["width"] = l.width;
      if (l.outline) lo["outline"] = json::array{(*l.outline)[0], (*l.outline)[1], (*l.outline)[2]};
      layers.push_back(lo);
      d.sources[l.name] = s;
    }
    basis["layers"] = layers;
    basis["extent"] = json::array{dc.extent[0], dc.extent[1], dc.extent[2], dc.extent[3]};
    d.version = sha256_hex(json::serialize(basis));
    if (dc.registration) d.registration = summarize_registration(*dc.registration);
    d.publication = nullptr;
    if (dc.publication) {
      boost::system::error_code ec;
      json::value pub = json::parse(read_file(*dc.publication, 1 << 20), ec);
      if (ec || !pub.is_object() || !pub.get_object().contains("stage") ||
          pub.get_object().at("stage") != "PUBLISHED")
        throw std::runtime_error("publication reference must be a persisted PUBLISHED record");
      json::object p;
      p["stage"] = "PUBLISHED";
      if (auto id = pub.get_object().if_contains("identity")) p["identity"] = *id;
      d.publication = p;
    }
    d.map_alias = dc.id + "_" + d.version.substr(0, 16);
    d.mapfile = maps / (d.map_alias + ".map");
    d.loaded_unix_ms = unix_ms();
    write_file_atomic(d.mapfile, render_mapfile(d));
    conf << "    " << d.map_alias << " \"" << d.mapfile.string() << "\"\n";
    next->datasets.emplace(dc.id, std::move(d));
  }
  conf << "  END\nEND\n";
  std::unique_lock lock(m_);
  next->generation = ++generation_;
  next->mapserver_config = config_.state_dir / ("mapserver-" + std::to_string(next->generation) + ".conf");
  write_file_atomic(next->mapserver_config, conf.str());
  // Remove mapfiles that no longer belong to the active generation.
  for (auto &entry : fs::directory_iterator(maps)) {
    bool active = false;
    for (auto &[id, d] : next->datasets) active |= entry.path() == d.mapfile;
    if (!active && entry.path().extension() == ".map") fs::remove(entry.path());
  }
  if (state_ && state_->mapserver_config != next->mapserver_config) {
    std::error_code ec;
    fs::remove(state_->mapserver_config, ec);
  }
  state_ = next;
  return next;
}

std::shared_ptr<const CatalogState> Catalog::current() const {
  std::shared_lock lock(m_);
  return state_;
}

bool Catalog::stale(const DatasetState &d) const {
  for (const auto &l : d.config->layers) {
    const SourceState &recorded = d.sources.at(l.name);
    SourceState now;
    try {
      now = stat_source(l.source);
    } catch (const Error &) {
      return true;
    }
    if (now.size == recorded.size && now.mtime_ns == recorded.mtime_ns && now.inode == recorded.inode)
      continue;
    // Metadata changed: only a different digest counts as a change of content.
    try {
      if (sha256_file_hex(l.source) != recorded.sha256) return true;
    } catch (const Error &) {
      return true;
    }
  }
  return false;
}

std::string Catalog::render_mapfile(const DatasetState &d) const {
  const DatasetConfig &c = *d.config;
  auto epsg = [](const std::string &crs) { return "init=epsg:" + crs.substr(5); };
  std::ostringstream m;
  m.precision(17);
  m << "MAP\n  NAME \"" << c.id << "\"\n  SIZE 512 512\n  MAXSIZE "
    << std::max(config_.limits.max_width, config_.limits.max_height) << "\n";
  m << "  EXTENT " << c.extent[0] << " " << c.extent[1] << " " << c.extent[2] << " " << c.extent[3] << "\n";
  m << "  UNITS METERS\n  PROJECTION \"" << epsg(c.crs) << "\" END\n";
  m << "  IMAGETYPE \"png\"\n";
  m << "  OUTPUTFORMAT NAME \"png\" DRIVER \"AGG/PNG\" MIMETYPE \"image/png\" IMAGEMODE RGBA TRANSPARENT ON EXTENSION \"png\" END\n";
  m << "  OUTPUTFORMAT NAME \"jpeg\" DRIVER \"AGG/JPEG\" MIMETYPE \"image/jpeg\" IMAGEMODE RGB EXTENSION \"jpg\" END\n";
  std::string srs;
  for (auto &s : c.allowed_crs) srs += (srs.empty() ? "" : " ") + s;
  m << "  WEB\n    METADATA\n      \"ows_title\" \"" << c.title << "\"\n"
    << "      \"ows_enable_request\" \"GetMap GetCapabilities\"\n"
    << "      \"ows_srs\" \"" << srs << "\"\n"
    << "      \"wms_feature_info_mime_type\" \"application/json\"\n"
    << "      \"ows_onlineresource\" \"" << base_url_ << "/v1/maps/" << c.id << "/wms?\"\n";
  if (!c.attribution.empty()) m << "      \"wms_attribution_title\" \"" << c.attribution << "\"\n";
  m << "    END\n  END\n";
  for (const auto &l : c.layers) {
    const char *type = l.kind == LayerKind::Raster ? "RASTER" : l.kind == LayerKind::Polygon ? "POLYGON"
                       : l.kind == LayerKind::Line ? "LINE" : "POINT";
    m << "  LAYER\n    NAME \"" << l.name << "\"\n    TYPE " << type << "\n    STATUS ON\n";
    if (l.kind == LayerKind::Raster) {
      m << "    DATA \"" << l.source.string() << "\"\n    PROCESSING \"SCALE=AUTO\"\n    PROCESSING \"RESAMPLE=BILINEAR\"\n";
    } else {
      m << "    CONNECTIONTYPE OGR\n    CONNECTION \"" << l.source.string() << "\"\n    DATA \""
        << l.source_layer << "\"\n";
    }
    m << "    PROJECTION \"" << epsg(l.crs) << "\" END\n";
    m << "    METADATA \"wms_title\" \"" << l.title << "\" \"wms_srs\" \"" << srs << "\" END\n";
    if (l.kind != LayerKind::Raster) {
      m << "    CLASS STYLE COLOR " << l.color[0] << " " << l.color[1] << " " << l.color[2];
      if (l.outline) m << " OUTLINECOLOR " << (*l.outline)[0] << " " << (*l.outline)[1] << " " << (*l.outline)[2];
      m << " WIDTH " << l.width << " END END\n";
    }
    m << "  END\n";
  }
  m << "END\n";
  return m.str();
}

json::object Catalog::describe(const DatasetState &d) const {
  const DatasetConfig &c = *d.config;
  json::object o;
  o["id"] = c.id;
  o["title"] = c.title;
  o["version"] = d.version;
  o["crs"] = c.crs;
  o["allowed_crs"] = json::value_from(c.allowed_crs);
  o["extent"] = json::array{c.extent[0], c.extent[1], c.extent[2], c.extent[3]};
  o["attribution"] = c.attribution;
  json::array layers;
  for (const auto &l : c.layers) {
    json::object lo;
    lo["name"] = l.name;
    lo["title"] = l.title;
    lo["type"] = layer_kind_name(l.kind);
    lo["crs"] = l.crs;
    lo["sha256"] = d.sources.at(l.name).sha256;
    lo["bytes"] = d.sources.at(l.name).size;
    layers.push_back(lo);
  }
  o["layers"] = layers;
  o["services"] = json::object{{"wms", json::array{"1.1.1", "1.3.0"}},
                               {"wms_endpoint", "/v1/maps/" + c.id + "/wms"},
                               {"versioned_endpoint", "/v1/maps/" + c.id + "@" + d.version + "/wms"},
                               {"spatial", json::array{"inspect", "nearby"}}};
  if (d.registration.empty())
    o["registration"] = json::object{{"status", "none"}, {"note", "2D geographic source projection only; no world registration is implied"}};
  else
    o["registration"] = d.registration;
  o["publication"] = d.publication;
  o["world_mutation"] = false;
  o["loaded_unix_ms"] = d.loaded_unix_ms;
  return o;
}

json::array Catalog::list() const {
  json::array out;
  auto s = current();
  for (auto &[id, d] : s->datasets) out.push_back(describe(d));
  return out;
}

}  // namespace fvmap
