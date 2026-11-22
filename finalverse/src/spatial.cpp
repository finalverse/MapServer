#include "spatial.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

#include <boost/url.hpp>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>

namespace fvmap {

namespace {

constexpr std::size_t kMaxFeatures = 100000;
constexpr std::size_t kMaxRasterCells = 4u << 20;

struct DatasetCloser {
  void operator()(GDALDataset *d) const { GDALClose(d); }
};
using DatasetPtr = std::unique_ptr<GDALDataset, DatasetCloser>;

std::unique_ptr<OGRSpatialReference> srs(const std::string &crs) {
  auto s = std::make_unique<OGRSpatialReference>();
  if (s->SetFromUserInput(crs.c_str()) != OGRERR_NONE) throw Error(400, "unsupported_crs", "CRS is unknown");
  s->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  return s;
}

std::unique_ptr<OGRCoordinateTransformation> transform(const OGRSpatialReference &a, const OGRSpatialReference &b) {
  std::unique_ptr<OGRCoordinateTransformation> ct(OGRCreateCoordinateTransformation(&a, &b));
  if (!ct) throw Error(400, "unsupported_crs", "no coordinate operation between CRSs");
  return ct;
}

void require_metric(const DatasetState &d, const OGRSpatialReference &s) {
  if (!s.IsProjected() || std::abs(s.GetLinearUnits() - 1.0) > 1e-12)
    throw Error(409, "not_metric", "dataset " + d.config->id + " is not in a projected metre CRS");
}

DatasetPtr open_vector(const LayerConfig &l) {
  static const char *drivers[] = {"GeoJSON", "GPKG", "FlatGeobuf", "ESRI Shapefile", nullptr};
  DatasetPtr ds(GDALDataset::Open(l.source.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, drivers));
  if (!ds) throw Error(503, "source_unreadable", "layer " + l.name + " cannot be opened");
  return ds;
}

std::array<double, 4> bounds_to(const std::array<double, 4> &b, OGRCoordinateTransformation &ct) {
  double x0, y0, x1, y1;
  if (!ct.TransformBounds(b[0], b[1], b[2], b[3], &x0, &y0, &x1, &y1, 21))
    throw Error(400, "transform_failed", "bounds cannot be transformed");
  return {x0, y0, x1, y1};
}

std::map<std::string, std::string> query_map(std::string_view raw, std::initializer_list<std::string_view> allowed) {
  if (raw.size() > 1024) throw Error(414, "query_too_long", "query string exceeds its bound");
  const std::string query_text(raw);  // owned, NUL-terminated
  auto parsed = boost::urls::parse_query(query_text);
  if (!parsed) throw Error(400, "malformed_query", "query string is not well formed");
  std::map<std::string, std::string> out;
  for (auto p : *parsed) {
    std::string k = p.key.decode();
    if (std::find(allowed.begin(), allowed.end(), k) == allowed.end())
      throw Error(400, "unsupported_parameter", "parameter is not accepted: " + k);
    if (!out.emplace(k, p.value.decode()).second) throw Error(400, "duplicate_parameter", "parameter repeated: " + k);
  }
  return out;
}

double number(const std::string &s, const char *what) {
  char *end = nullptr;
  double v = std::strtod(s.c_str(), &end);
  if (s.empty() || end != s.c_str() + s.size() || !std::isfinite(v))
    throw Error(400, "invalid_parameter", std::string(what) + " must be a finite number");
  return v;
}

std::string crs_param(const std::map<std::string, std::string> &q, const DatasetState &d) {
  std::string crs = q.contains("crs") ? q.at("crs") : d.config->crs;
  if (std::find(d.config->allowed_crs.begin(), d.config->allowed_crs.end(), crs) == d.config->allowed_crs.end())
    throw Error(400, "unsupported_crs", "CRS is not offered by this dataset");
  return crs;
}

double round6(double v) { return std::round(v * 1e6) / 1e6; }

}  // namespace

InspectQuery parse_inspect(std::string_view raw, const DatasetState &d) {
  auto q = query_map(raw, {"bbox", "crs"});
  InspectQuery r;
  r.crs = crs_param(q, d);
  if (!q.contains("bbox")) throw Error(400, "missing_parameter", "bbox is required");
  std::string b = q["bbox"];
  std::array<double, 4> v{};
  std::size_t start = 0;
  for (int i = 0; i < 4; ++i) {
    std::size_t end = b.find(',', start);
    if ((i < 3) != (end != std::string::npos)) throw Error(400, "invalid_bbox", "bbox must have four values");
    v[i] = number(b.substr(start, end == std::string::npos ? std::string::npos : end - start), "bbox");
    start = end + 1;
  }
  if (v[0] >= v[2] || v[1] >= v[3]) throw Error(400, "invalid_bbox", "bbox must be ordered");
  r.bbox = v;
  return r;
}

NearbyQuery parse_nearby(std::string_view raw, const DatasetState &d) {
  auto q = query_map(raw, {"x", "y", "crs", "radius", "layer", "limit"});
  NearbyQuery r;
  r.crs = crs_param(q, d);
  if (!q.contains("x") || !q.contains("y")) throw Error(400, "missing_parameter", "x and y are required");
  r.x = number(q["x"], "x");
  r.y = number(q["y"], "y");
  if (q.contains("radius")) r.radius_m = number(q["radius"], "radius");
  if (!(r.radius_m > 0 && r.radius_m <= 2000)) throw Error(400, "invalid_parameter", "radius must be in (0, 2000] metres");
  if (q.contains("limit")) {
    double l = number(q["limit"], "limit");
    if (l < 1 || l > 100 || l != std::floor(l)) throw Error(400, "invalid_parameter", "limit must be 1..100");
    r.limit = static_cast<int>(l);
  }
  if (q.contains("layer")) {
    bool known = false;
    for (auto &l : d.config->layers) known |= l.name == q["layer"] && l.kind != LayerKind::Raster;
    if (!known) throw Error(404, "unknown_layer", "vector layer is not in this dataset");
    r.layer = q["layer"];
  }
  return r;
}

json::object spatial_inspect(const DatasetState &d, const InspectQuery &q) {
  auto metric = srs(d.config->crs);
  require_metric(d, *metric);
  auto req = srs(q.crs);
  auto to_metric = transform(*req, *metric);
  auto box = bounds_to(q.bbox, *to_metric);
  double area = (box[2] - box[0]) * (box[3] - box[1]);
  if (area > 25e6) throw Error(400, "bbox_too_large", "inspection is limited to 25 km²");
  OGRPolygon clip;
  {
    OGRLinearRing ring;
    ring.addPoint(box[0], box[1]);
    ring.addPoint(box[2], box[1]);
    ring.addPoint(box[2], box[3]);
    ring.addPoint(box[0], box[3]);
    ring.addPoint(box[0], box[1]);
    clip.addRing(&ring);
  }
  json::array layers;
  for (const auto &l : d.config->layers) {
    json::object lo{{"name", l.name}, {"type", layer_kind_name(l.kind)}};
    auto lsrs = srs(l.crs);
    auto from_metric = transform(*metric, *lsrs);
    auto lbox = bounds_to(box, *from_metric);
    if (l.kind == LayerKind::Raster) {
      DatasetPtr ds(GDALDataset::Open(l.source.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
      if (!ds || ds->GetRasterCount() < 1) throw Error(503, "source_unreadable", "raster cannot be opened");
      double gt[6];
      if (ds->GetGeoTransform(gt) != CE_None || gt[2] != 0 || gt[4] != 0)
        throw Error(409, "unsupported_raster", "raster must be north-up");
      int nx = ds->GetRasterXSize(), ny = ds->GetRasterYSize();
      auto col = [&](double x) { return (x - gt[0]) / gt[1]; };
      auto row = [&](double y) { return (y - gt[3]) / gt[5]; };
      int c0 = std::clamp(static_cast<int>(std::floor(std::min(col(lbox[0]), col(lbox[2])))), 0, nx);
      int c1 = std::clamp(static_cast<int>(std::ceil(std::max(col(lbox[0]), col(lbox[2])))), 0, nx);
      int r0 = std::clamp(static_cast<int>(std::floor(std::min(row(lbox[1]), row(lbox[3])))), 0, ny);
      int r1 = std::clamp(static_cast<int>(std::ceil(std::max(row(lbox[1]), row(lbox[3])))), 0, ny);
      std::size_t cells = static_cast<std::size_t>(std::max(0, c1 - c0)) * std::max(0, r1 - r0);
      if (cells > kMaxRasterCells) throw Error(400, "bbox_too_large", "raster window exceeds its bound");
      lo["cells"] = cells;
      if (cells > 0) {
        std::vector<double> buf(cells);
        GDALRasterBand *band = ds->GetRasterBand(1);
        if (band->RasterIO(GF_Read, c0, r0, c1 - c0, r1 - r0, buf.data(), c1 - c0, r1 - r0, GDT_Float64, 0, 0) != CE_None)
          throw Error(503, "source_unreadable", "raster read failed");
        int has_nodata = 0;
        double nodata = band->GetNoDataValue(&has_nodata);
        double mn = INFINITY, mx = -INFINITY, sum = 0;
        std::size_t valid = 0;
        for (double v : buf) {
          if ((has_nodata && v == nodata) || !std::isfinite(v)) continue;
          mn = std::min(mn, v), mx = std::max(mx, v), sum += v, ++valid;
        }
        lo["valid_cells"] = valid;
        if (valid) {
          lo["min"] = round6(mn);
          lo["max"] = round6(mx);
          lo["mean"] = round6(sum / valid);
        }
        lo["cell_size"] = json::array{std::abs(gt[1]), std::abs(gt[5])};
        lo["unit"] = "source raster values (see dataset vertical datum metadata)";
      }
    } else {
      DatasetPtr ds = open_vector(l);
      OGRLayer *layer = ds->GetLayerByName(l.source_layer.c_str());
      if (!layer) throw Error(503, "source_unreadable", "source layer is missing");
      layer->SetSpatialFilterRect(lbox[0], lbox[1], lbox[2], lbox[3]);
      layer->ResetReading();
      auto to_m = transform(*lsrs, *metric);
      std::size_t count = 0, scanned = 0;
      double length = 0, area_m2 = 0;
      for (auto &f : *layer) {
        if (++scanned > kMaxFeatures) throw Error(400, "too_many_features", "feature scan exceeds its bound");
        OGRGeometry *g = f->GetGeometryRef();
        if (!g) continue;
        std::unique_ptr<OGRGeometry> gm(g->clone());
        if (gm->transform(to_m.get()) != OGRERR_NONE) continue;
        if (!gm->Intersects(&clip)) continue;
        std::unique_ptr<OGRGeometry> part(gm->Intersection(&clip));
        if (!part || part->IsEmpty()) continue;
        ++count;
        OGRGeometryH h = OGRGeometry::ToHandle(part.get());
        length += OGR_G_Length(h);  // 0 for areal parts
        area_m2 += OGR_G_Area(h);   // 0 for linear parts
      }
      lo["features"] = count;
      if (l.kind == LayerKind::Line) lo["length_m"] = round6(length);
      if (l.kind == LayerKind::Polygon) lo["area_m2"] = round6(area_m2);
    }
    layers.push_back(lo);
  }
  return {{"operation", "spatial.inspect_region"},
          {"dataset", d.config->id},
          {"dataset_version", d.version},
          {"bbox_metric", json::array{round6(box[0]), round6(box[1]), round6(box[2]), round6(box[3])}},
          {"metric_crs", d.config->crs},
          {"layers", layers},
          {"deterministic", true}};
}

json::object spatial_nearby(const DatasetState &d, const NearbyQuery &q) {
  auto metric = srs(d.config->crs);
  require_metric(d, *metric);
  auto req = srs(q.crs);
  auto to_metric = transform(*req, *metric);
  double px = q.x, py = q.y;
  if (!to_metric->Transform(1, &px, &py)) throw Error(400, "transform_failed", "point cannot be transformed");
  OGRPoint p(px, py);
  struct Hit {
    double dist;
    std::string layer;
    int64_t fid;
    json::object attrs;
    std::string type;
  };
  std::vector<Hit> hits;
  for (const auto &l : d.config->layers) {
    if (l.kind == LayerKind::Raster || (q.layer && *q.layer != l.name)) continue;
    auto lsrs = srs(l.crs);
    auto from_metric = transform(*metric, *lsrs);
    auto to_m = transform(*lsrs, *metric);
    auto lbox = bounds_to({px - q.radius_m, py - q.radius_m, px + q.radius_m, py + q.radius_m}, *from_metric);
    DatasetPtr ds = open_vector(l);
    OGRLayer *layer = ds->GetLayerByName(l.source_layer.c_str());
    if (!layer) throw Error(503, "source_unreadable", "source layer is missing");
    layer->SetSpatialFilterRect(lbox[0], lbox[1], lbox[2], lbox[3]);
    std::size_t scanned = 0;
    for (auto &f : *layer) {
      if (++scanned > kMaxFeatures) throw Error(400, "too_many_features", "feature scan exceeds its bound");
      OGRGeometry *g = f->GetGeometryRef();
      if (!g) continue;
      std::unique_ptr<OGRGeometry> gm(g->clone());
      if (gm->transform(to_m.get()) != OGRERR_NONE) continue;
      double dist = gm->Distance(&p);
      if (dist < 0 || dist > q.radius_m) continue;
      json::object attrs;
      int shown = 0;
      for (int i = 0; i < f->GetFieldCount() && shown < 8; ++i) {
        if (!f->IsFieldSetAndNotNull(i)) continue;
        std::string v = f->GetFieldAsString(i);
        if (v.size() > 128) v.resize(128);
        attrs[f->GetFieldDefnRef(i)->GetNameRef()] = v;
        ++shown;
      }
      hits.push_back({round6(dist), l.name, f->GetFID(), std::move(attrs), OGRGeometryTypeToName(gm->getGeometryType())});
    }
  }
  std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
    return std::tie(a.dist, a.layer, a.fid) < std::tie(b.dist, b.layer, b.fid);
  });
  json::array out;
  for (std::size_t i = 0; i < hits.size() && i < static_cast<std::size_t>(q.limit); ++i)
    out.push_back({{"layer", hits[i].layer}, {"fid", hits[i].fid}, {"distance_m", hits[i].dist},
                   {"geometry_type", hits[i].type}, {"attributes", hits[i].attrs}});
  return {{"operation", "spatial.find_nearby"},
          {"dataset", d.config->id},
          {"dataset_version", d.version},
          {"point_metric", json::array{round6(px), round6(py)}},
          {"metric_crs", d.config->crs},
          {"radius_m", q.radius_m},
          {"matches", hits.size()},
          {"features", out},
          {"deterministic", true}};
}

}  // namespace fvmap
