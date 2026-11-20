# Finalverse Map Server (`fvmapd`)

Finalverse Map Server is the geographic publication and spatial service gateway of Finalverse. It is a native C++23 service in front of an **unmodified** MapServer CGI backend: this directory adds a product, and the MapServer sources in this repository are untouched. It never acts as a world authority. World data is read from MutSea through the read-only native projection.

What it provides: a versioned map catalog, strict WMS 1.1.1/1.3.0 routing, CRS-aware validation, a persistent content-verified cache, bearer-token scopes, rate and resource bounds, Prometheus metrics, JSON logs, readiness and restart recovery, the deterministic spatial operations `spatial.inspect_region` and `spatial.find_nearby`, and (optionally) the World Scene API.

## Build

```sh
cmake -S finalverse -B /tmp/fvmap -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="/opt/homebrew;/path/to/gdal-prefix" -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3
cmake --build /tmp/fvmap
/tmp/fvmap/fvmap_unit_tests
```

Requirements: Boost ≥ 1.84 (Beast, JSON, URL), OpenSSL 3, GDAL ≥ 3.8 with PROJ and GEOS.

Optional settings:

- `-DFVMAP_WITH_MUTSEA=ON` with an installed MutSeaNative ≥ 0.3 enables `/v1/worlds/*`.
- `-DFVMAP_SANITIZERS=ON` builds with ASan/UBSan.
- `-DFVMAP_FUZZ=ON` with LLVM clang builds `fvmap_fuzz_wms`.

Build the backend from this repository, unmodified, with WMS enabled, for example:

```sh
cmake -S . -B /tmp/ms -G Ninja -DCMAKE_BUILD_TYPE=Release -DWITH_WMS=ON -DWITH_OGCAPI=ON -DWITH_LIBXML2=ON \
  -DWITH_CAIRO=OFF -DWITH_FCGI=OFF -DWITH_POSTGIS=OFF -DWITH_PROTOBUFC=OFF -DWITH_HARFBUZZ=OFF -DWITH_FRIBIDI=OFF
cmake --build /tmp/ms --target mapserv
```

## Run

```sh
fvmapd new-token                      # prints a token and its sha256; store only the digest
fvmapd check-config --config deploy/config.json
fvmapd serve --config deploy/config.json   # prints "READY <port>"; SIGHUP reloads, SIGTERM stops
```

The configuration schema is `finalverse.map-server-config/1` (see `config/montreal.example.json`); tokens use `finalverse.map-server-tokens/1`, and the token file must be mode 0600. FinalGIS writes complete private deployments with `finalgis/scripts/map_server_config.py`. The server binds to loopback unless `allow_non_loopback` is set explicitly.

## Test

```sh
python3 finalverse/tests/integration.py --fvmapd /tmp/fvmap/fvmapd --mapserv /tmp/ms/mapserv \
  --source /path/to/finalgis/fixtures/montreal --env-prefix /path/to/gdal-prefix \
  [--world-store /copy/of/region.sqlite --world-registration /path/geographic-registration.json] [--out report.json]
```

Results in Ω-XVII.2: 63/63 checks against this fork (8.7-dev), upstream rel-8-6-6 and an ASan build; 73/73 with the world API. Architecture, security and performance notes are in FinalGIS `docs/omega-xvii-2/`.

The MapServer license (MIT-style, `LICENSE.md`) is unchanged and applies to the backend. The gateway is Finalverse code.
