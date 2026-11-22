#!/usr/bin/env python3
"""End-to-end checks of fvmapd against a real, unmodified MapServer backend.

Everything runs on 127.0.0.1 inside a private temporary directory; the
reference dataset is copied first so source-change tests never touch it.
"""

import argparse
import concurrent.futures
import hashlib
import json
import os
import shutil
import signal
import socket
import stat
import statistics
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zlib
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument("--fvmapd", required=True)
p.add_argument("--mapserv", required=True)
p.add_argument("--source", required=True, type=Path)
p.add_argument("--env-prefix", default="")
p.add_argument("--world-store", default="")
p.add_argument("--world-registration", default="")
p.add_argument("--out", default="")
a = p.parse_args()
a.fvmapd = os.path.abspath(a.fvmapd)
a.mapserv = os.path.abspath(a.mapserv)
a.source = a.source.resolve()

opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
results = {"checks": [], "measurements": {}}
failures = []


def check(name, cond, detail=""):
    results["checks"].append({"name": name, "pass": bool(cond), "detail": str(detail)[:300]})
    if not cond:
        failures.append(name)
        print(f"FAIL {name}: {detail}", file=sys.stderr)


def req(base, path, token=None, method="GET", timeout=30):
    r = urllib.request.Request(base + path, method=method)
    if token:
        r.add_header("Authorization", "Bearer " + token)
    t0 = time.perf_counter()
    try:
        with opener.open(r, timeout=timeout) as resp:
            body = resp.read()
            return resp.status, dict(resp.headers), body, time.perf_counter() - t0
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read(), time.perf_counter() - t0


def png_ok(body, w, h):
    if not body.startswith(b"\x89PNG\r\n\x1a\n"):
        return False, "not png"
    pos, idat, width, height = 8, b"", 0, 0
    while pos < len(body):
        n = int.from_bytes(body[pos:pos + 4], "big")
        kind = body[pos + 4:pos + 8]
        data = body[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            width, height = int.from_bytes(data[:4], "big"), int.from_bytes(data[4:8], "big")
        if kind == b"IDAT":
            idat += data
        pos += 12 + n
    raw = zlib.decompress(idat)
    distinct = len(set(raw[1:4096 * 4]))
    return (width, height) == (w, h) and distinct > 4, f"{width}x{height} distinct={distinct}"


tmp = Path(tempfile.mkdtemp(prefix="fvmap-it-"))
os.chmod(tmp, 0o700)
src = tmp / "source"
shutil.copytree(a.source, src)
tokens = {name: hashlib.sha256(os.urandom(32)).hexdigest() for name in ["admin", "viewer", "stranger", "burst"]}
token_doc = {
    "schema": "finalverse.map-server-tokens/1",
    "principals": [
        {"principal": "admin", "token_sha256": hashlib.sha256(tokens["admin"].encode()).hexdigest(), "role": "admin", "datasets": ["*"], "worlds": ["*"]},
        {"principal": "viewer", "token_sha256": hashlib.sha256(tokens["viewer"].encode()).hexdigest(), "role": "reader", "datasets": ["montreal"], "worlds": ["montreal-episode"]},
        {"principal": "stranger", "token_sha256": hashlib.sha256(tokens["stranger"].encode()).hexdigest(), "role": "reader", "datasets": [], "worlds": []},
        {"principal": "burst", "token_sha256": hashlib.sha256(tokens["burst"].encode()).hexdigest(), "role": "reader", "datasets": ["montreal"]},
    ],
}
token_file = tmp / "tokens.json"
token_file.write_text(json.dumps(token_doc))
os.chmod(token_file, 0o600)
env = {}
if a.env_prefix:
    env["PROJ_DATA"] = str(Path(a.env_prefix) / "share/proj")
extent = [300489.334946641, 5040679.30754108, 300749.334946641, 5040939.30754108]


def make_config(state, backend, limits=None, backend_extra=None):
    cfg = {
        "schema": "finalverse.map-server-config/1",
        "listen": {"host": "127.0.0.1", "port": 0},
        "state_dir": str(state),
        "tokens": str(token_file),
        "backend": dict({"mapserv": backend, "env": env, "timeout_ms": 15000, "max_concurrency": 4}, **(backend_extra or {})),
        "limits": dict({"worker_threads": 8, "rate_per_second": 500, "burst": 1000, "queue_timeout_ms": 5000}, **(limits or {})),
        "cache": {"max_bytes": 64 << 20, "max_entries": 4096},
        "datasets": [{
            "id": "montreal",
            "title": "Montreal Old Port reference",
            "crs": "EPSG:2950",
            "extent": extent,
            "allowed_crs": ["EPSG:2950", "EPSG:4326", "EPSG:3857"],
            "attribution": "Ville de Montreal CC-BY-4.0; NRCan OGL-Canada-2.0",
            "layers": [
                {"name": "terrain", "type": "raster", "source": str(src / "dtm.tif"), "crs": "EPSG:3979"},
                {"name": "roofs", "type": "polygon", "source": str(src / "buildings.geojson"), "source_layer": "buildings", "crs": "EPSG:2950", "color": [188, 128, 87], "outline": [73, 63, 59]},
                {"name": "roads", "type": "line", "source": str(src / "roads.geojson"), "source_layer": "roads", "crs": "EPSG:4326", "color": [36, 132, 155], "width": 2},
            ],
        }],
    }
    if a.world_store:
        world = {"id": "montreal-episode", "title": "Montreal episode (copied store)", "dataset": "montreal", "store": a.world_store}
        if a.world_registration:
            world["registration"] = a.world_registration
        cfg["worlds"] = [world]
    path = state.parent / (state.name + ".json")
    path.write_text(json.dumps(cfg, indent=1))
    return path


def start(config, log):
    proc = subprocess.Popen([a.fvmapd, "serve", "--config", str(config)], stdout=subprocess.PIPE, stderr=log, text=True)
    line = proc.stdout.readline()
    if not line.startswith("READY "):
        proc.kill()
        raise RuntimeError("fvmapd did not start: " + line)
    return proc, f"http://127.0.0.1:{int(line.split()[1])}"


def stop(proc):
    proc.send_signal(signal.SIGTERM)
    return proc.wait(timeout=20)


state = tmp / "state"
config = make_config(state, a.mapserv)
log = (tmp / "fvmapd.log").open("w")
t0 = time.perf_counter()
proc, base = start(config, log)
results["measurements"]["startup_seconds"] = round(time.perf_counter() - t0, 4)
V, ADM, S = tokens["viewer"], tokens["admin"], tokens["stranger"]

# Health and authentication.
st, _, body, _ = req(base, "/healthz")
check("healthz", st == 200 and json.loads(body)["status"] == "ok", body)
st, _, body, _ = req(base, "/readyz")
check("readyz", st == 200 and json.loads(body)["ready"], body)
st, h, body, _ = req(base, "/v1/catalog")
check("catalog requires token", st == 401 and "Bearer" in h.get("WWW-Authenticate", ""), st)
st, _, _, _ = req(base, "/v1/catalog", "x" * 64)
check("catalog rejects unknown token", st == 401, st)
st, _, body, _ = req(base, "/v1/catalog", V)
cat = json.loads(body)
ds = cat["datasets"][0]
version = ds["version"]
check("catalog lists dataset", st == 200 and ds["id"] == "montreal" and len(version) == 64 and [l["name"] for l in ds["layers"]] == ["terrain", "roofs", "roads"], body[:300])
check("catalog source digests", {l["name"]: l["sha256"] for l in ds["layers"]} == {
    n: hashlib.sha256((src / f).read_bytes()).hexdigest() for n, f in [("terrain", "dtm.tif"), ("roofs", "buildings.geojson"), ("roads", "roads.geojson")]}, ds["layers"])
check("catalog backend identity", cat["backend"]["sha256"] == hashlib.sha256(Path(a.mapserv).read_bytes()).hexdigest(), cat["backend"])
check("catalog registration honest", ds["registration"]["status"] == "none" and ds["world_mutation"] is False, ds["registration"])
st, _, body, _ = req(base, "/v1/catalog", S)
check("catalog scoped per principal", st == 200 and json.loads(body)["datasets"] == [], body)
st, _, _, _ = req(base, "/v1/catalog/montreal", S)
check("dataset hidden from stranger", st == 404, st)

# WMS capabilities.
for ver in ["1.1.1", "1.3.0"]:
    st, h, body, _ = req(base, f"/v1/maps/montreal/wms?SERVICE=WMS&VERSION={ver}&REQUEST=GetCapabilities", V)
    text = body.decode(errors="replace")
    check(f"GetCapabilities {ver}", st == 200 and "xml" in h.get("Content-Type", "") and all(f"<Name>{n}</Name>" in text for n in ["roofs", "roads", "terrain"])
          and f"{base}/v1/maps/montreal/wms?" in text and "/Users/" not in text, text[:300])

# GetMap in the native CRS, cold then cached.
q2950 = "SERVICE=WMS&VERSION=1.1.1&REQUEST=GetMap&LAYERS=terrain,roofs,roads&STYLES=,,&SRS=EPSG:2950&BBOX=" + ",".join(map(str, extent)) + "&WIDTH=512&HEIGHT=512&FORMAT=image/png"
st, h, cold, t_cold = req(base, "/v1/maps/montreal/wms?" + q2950, V)
ok, why = png_ok(cold, 512, 512)
check("GetMap EPSG:2950 renders", st == 200 and h.get("X-Cache") == "MISS" and ok and h.get("X-Dataset-Version") == version, (st, h.get("X-Cache"), why))
(tmp / "preview-2950.png").write_bytes(cold)
st, h, hot, t_hot = req(base, "/v1/maps/montreal/wms?" + q2950, V)
check("GetMap cache hit identical", st == 200 and h.get("X-Cache") == "HIT" and hot == cold, h.get("X-Cache"))
reordered = "format=image/png&height=512&width=512&bbox=" + ",".join(map(str, extent)) + "&srs=EPSG:2950&styles=,,&layers=terrain,roofs,roads&request=GetMap&version=1.1.1&service=WMS"
st, h, again, _ = req(base, "/v1/maps/montreal/wms?" + reordered, V)
check("canonical query shares cache", st == 200 and h.get("X-Cache") == "HIT" and again == cold, h.get("X-Cache"))
results["measurements"]["getmap_cold_seconds"] = round(t_cold, 4)
results["measurements"]["getmap_hit_seconds"] = round(t_hot, 4)
results["measurements"]["png_bytes"] = len(cold)

for name, path in [
    ("GetMap EPSG:4326 1.1.1", "SERVICE=WMS&VERSION=1.1.1&REQUEST=GetMap&LAYERS=roofs,roads&STYLES=&SRS=EPSG:4326&BBOX=-73.5552,45.5060,-73.5518,45.5084&WIDTH=400&HEIGHT=300&FORMAT=image/png"),
    ("GetMap EPSG:4326 1.3.0 lat/lon", "SERVICE=WMS&VERSION=1.3.0&REQUEST=GetMap&LAYERS=roofs,roads&STYLES=&CRS=EPSG:4326&BBOX=45.5060,-73.5552,45.5084,-73.5518&WIDTH=400&HEIGHT=300&FORMAT=image/png"),
    ("GetMap EPSG:3857 jpeg", "SERVICE=WMS&VERSION=1.3.0&REQUEST=GetMap&LAYERS=terrain,roofs&STYLES=&CRS=EPSG:3857&BBOX=-8188120,5701540,-8187750,5701910&WIDTH=256&HEIGHT=256&FORMAT=image/jpeg"),
]:
    st, h, body, _ = req(base, "/v1/maps/montreal/wms?" + path, V)
    if "jpeg" in name:
        check(name, st == 200 and body.startswith(b"\xff\xd8") and len(body) > 2000, (st, body[:120]))
    else:
        ok, why = png_ok(body, 400, 300)
        check(name, st == 200 and ok, (st, why, body[:200]))
        if "1.1.1" in name:
            v111 = body
        else:
            check("1.1.1 and 1.3.0 axis orders render the same map", body == v111, "differs")

# Refusals.
refusals = [
    ("map injection", q2950 + "&MAP=/etc/passwd", 400, "unsupported_parameter"),
    ("duplicate param", q2950 + "&WIDTH=4", 400, "duplicate_parameter"),
    ("unsupported CRS", q2950.replace("EPSG:2950", "EPSG:32618"), 400, "unsupported_crs"),
    ("oversize", q2950.replace("WIDTH=512", "WIDTH=99999"), 400, "invalid_size"),
    ("unknown layer", q2950.replace("LAYERS=terrain,roofs,roads&STYLES=,,", "LAYERS=secret&STYLES="), 404, "unknown_layer"),
    ("outside extent", q2950.replace(",".join(map(str, extent)), "0,0,100,100"), 400, "outside_extent"),
    ("wfs refused", "SERVICE=WFS&REQUEST=GetCapabilities", 400, "unsupported_service"),
    ("control chars", q2950 + "&TRANSPARENT=%0D%0ATRUE", 400, "malformed_query"),
]
for name, q, want, code in refusals:
    st, _, body, _ = req(base, "/v1/maps/montreal/wms?" + q, V)
    try:
        got = json.loads(body)["error"]["code"]
    except Exception:
        got = body[:100]
    check("refuse " + name, st == want and got == code, (st, got))
st, _, _, _ = req(base, "/v1/maps/montreal/wms?" + q2950, S)
check("stranger cannot render", st == 404, st)
st, h, body, _ = req(base, f"/v1/maps/montreal@{version}/wms?" + q2950, V)
check("pinned current version", st == 200 and body == cold, st)
st, _, body, _ = req(base, "/v1/maps/montreal@" + "0" * 64 + "/wms?" + q2950, V)
check("pinned retired version", st == 410, st)
for path in ["/v1/maps/../etc/wms", "/v1/maps/montreal/%2e%2e/wms", "/v1//catalog", "/v1/nothing"]:
    st, _, _, _ = req(base, path, V)
    check("path " + path, st in (400, 404), st)

# Deterministic spatial operations.
st, _, body, _ = req(base, "/v1/spatial/montreal/inspect?bbox=" + ",".join(map(str, extent)), V)
insp = json.loads(body)
layers = {l["name"]: l for l in insp.get("layers", [])}
check("spatial.inspect_region", st == 200 and layers["roofs"]["features"] > 0 and layers["roofs"]["area_m2"] > 0 and layers["roads"]["length_m"] > 0
      and layers["terrain"]["valid_cells"] > 0, body[:400])
st2, _, body2, _ = req(base, "/v1/spatial/montreal/inspect?bbox=" + ",".join(map(str, extent)), V)
check("spatial.inspect_region deterministic", body2 == body, "differs")
cx, cy = (extent[0] + extent[2]) / 2, (extent[1] + extent[3]) / 2
st, _, body, _ = req(base, f"/v1/spatial/montreal/nearby?x={cx}&y={cy}&radius=60&limit=5", V)
near = json.loads(body)
check("spatial.find_nearby", st == 200 and 0 < len(near["features"]) <= 5 and all(f["distance_m"] <= 60 for f in near["features"])
      and [f["distance_m"] for f in near["features"]] == sorted(f["distance_m"] for f in near["features"]), body[:400])
st, _, body, _ = req(base, "/v1/spatial/montreal/nearby?x=-73.5535&y=45.5072&crs=EPSG:4326&radius=60&limit=5", V)
check("spatial.find_nearby geographic input", st == 200 and len(json.loads(body)["features"]) > 0, body[:200])
st, _, _, _ = req(base, f"/v1/spatial/montreal/nearby?x={cx}&y={cy}&radius=99999", V)
check("spatial radius bounded", st == 400, st)
results["measurements"]["inspect_layers"] = {k: {kk: vv for kk, vv in v.items() if kk != "name"} for k, v in layers.items()}

# Metrics and administration.
st, _, _, _ = req(base, "/metrics", V)
check("metrics need admin", st == 403, st)
st, _, body, _ = req(base, "/metrics", ADM)
text = body.decode()
check("metrics exposition", st == 200 and 'fvmap_cache_requests_total{result="hit"}' in text and "fvmap_backend_seconds_bucket" in text, text[:200])
st, _, _, _ = req(base, "/v1/admin/reload", V, method="POST")
check("reload needs admin", st == 403, st)

# Malformed HTTP on a raw socket.
host, port = "127.0.0.1", int(base.rsplit(":", 1)[1])
for name, payload, want in [
    ("garbage request line", b"\x00\x01GARBAGE\r\n\r\n", b" 400 "),
    ("oversized header", b"GET /healthz HTTP/1.1\r\nHost: x\r\nX-Pad: " + b"a" * 20000 + b"\r\n\r\n", b" 431 "),
    ("unsupported method", b"DELETE /v1/catalog HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer " + V.encode() + b"\r\n\r\n", b" 405 "),
]:
    s = socket.create_connection((host, port), timeout=10)
    s.sendall(payload)
    data = s.recv(4096)
    s.close()
    check("http " + name, want in data.split(b"\r\n")[0], data[:80])

# Concurrency: distinct requests across 4 backend slots.
def tile(i):
    dx = (extent[2] - extent[0]) / 4
    x0 = extent[0] + (i % 4) * dx
    y0 = extent[1] + (i // 4) * dx
    q = f"SERVICE=WMS&VERSION=1.1.1&REQUEST=GetMap&LAYERS=roofs,roads&STYLES=,&SRS=EPSG:2950&BBOX={x0},{y0},{x0 + dx},{y0 + dx}&WIDTH=256&HEIGHT=256&FORMAT=image/png"
    return req(base, "/v1/maps/montreal/wms?" + q, V)

t0 = time.perf_counter()
with concurrent.futures.ThreadPoolExecutor(16) as pool:
    tiles = list(pool.map(tile, range(16)))
elapsed = time.perf_counter() - t0
check("16 concurrent tiles", all(t[0] == 200 for t in tiles), [t[0] for t in tiles])
results["measurements"]["concurrent_16_tiles_seconds"] = round(elapsed, 4)
results["measurements"]["tile_latency_p50_seconds"] = round(statistics.median(t[3] for t in tiles), 4)
with concurrent.futures.ThreadPoolExecutor(16) as pool:
    t0 = time.perf_counter()
    hits = list(pool.map(tile, list(range(16)) * 8))
    elapsed = time.perf_counter() - t0
check("128 cached tiles", all(t[0] == 200 and t[1].get("X-Cache") == "HIT" for t in hits), [t[0] for t in hits][:10])
results["measurements"]["cached_requests_per_second"] = round(len(hits) / elapsed, 1)

# Restart recovery: same version, cache recovered, identical bytes.
check("graceful stop", stop(proc) == 0, "exit")
proc, base = start(config, log)
st, _, body, _ = req(base, "/v1/catalog/montreal", V)
check("restart keeps dataset version", json.loads(body)["version"] == version, body[:200])
st, h, body, _ = req(base, "/v1/maps/montreal/wms?" + q2950, V)
check("restart serves recovered cache", st == 200 and h.get("X-Cache") == "HIT" and body == cold, h.get("X-Cache"))
st, h, body, _ = req(base, "/v1/admin/cache/purge", ADM, method="POST")
st, h, body, _ = req(base, "/v1/maps/montreal/wms?" + q2950, V)
check("re-render after purge is byte-identical", st == 200 and h.get("X-Cache") == "MISS" and body == cold, h.get("X-Cache"))

# Source change: refused until reload, then a new version and fresh cache.
roads = src / "roads.geojson"
roads.write_bytes(roads.read_bytes() + b"\n")
st, _, body, _ = req(base, "/v1/maps/montreal/wms?" + q2950, V)
check("changed source refused", st == 409 and json.loads(body)["error"]["code"] == "dataset_changed", body[:200])
st, _, body, _ = req(base, "/readyz")
check("readyz reports change", st == 503, body)
st, _, body, _ = req(base, "/v1/admin/reload", ADM, method="POST")
new_version = json.loads(body)["datasets"][0]["version"]
check("reload publishes new version", st == 200 and new_version != version, (st, new_version))
st, h, body, _ = req(base, "/v1/maps/montreal/wms?" + q2950, V)
check("new version renders fresh", st == 200 and h.get("X-Cache") == "MISS" and h.get("X-Dataset-Version") == new_version, h)
st, _, _, _ = req(base, f"/v1/maps/montreal@{version}/wms?" + q2950, V)
check("old version retired", st == 410, st)

# World API (MutSea projection) when configured.
if a.world_store:
    st, _, body, _ = req(base, "/v1/worlds", V)
    worlds = json.loads(body)
    check("worlds listed", st == 200, body[:300])
    if worlds.get("projection"):
        st, _, body, _ = req(base, "/v1/worlds/montreal-episode", V)
        head = json.loads(body)
        check("world head", st == 200 and head.get("worldline", {}).get("verified") is True, body[:300])
        st, _, body, _ = req(base, "/v1/worlds/montreal-episode/snapshot", V)
        snap = json.loads(body)
        check("world snapshot", st == 200 and snap.get("entities") is not None and snap.get("revision") == head.get("revision"), body[:300])
        results["measurements"]["world_snapshot_bytes"] = len(body)
        n = head["worldline"]["length"]
        after = max(0, n - 5)
        st, _, body, _ = req(base, f"/v1/worlds/montreal-episode/events?after={after}&limit=10", V)
        ev = json.loads(body)
        check("world events tail", st == 200 and len(ev["events"]) == n - after and ev["events"][-1]["hash"] == head["worldline"]["hash"], body[:300])
        if after > 0:
            prev_hash = ev["events"][0]["prev_hash"]
            st, _, body, _ = req(base, f"/v1/worlds/montreal-episode/events?after={after}&after_hash={prev_hash}&limit=10", V)
            check("world events with matching cursor hash", st == 200, st)
            st, _, body, _ = req(base, f"/v1/worlds/montreal-episode/events?after={after}&after_hash={'0' * 64}&limit=10", V)
            check("world events detect forked cursor", st == 409 and json.loads(body)["error"]["code"] == "resync_required", body[:200])
        st, _, body, _ = req(base, f"/v1/worlds/montreal-episode/events?after={n + 10}", V)
        check("world events detect future cursor", st == 409, st)
        st, h, body, _ = req(base, f"/v1/worlds/montreal-episode/stream?after={after}&limit=10", V)
        check("world FVP stream", st == 200 and h.get("Content-Type") == "application/x-finalverse-fvp" and len(body) > 48 * (n - after), (st, len(body)))
        st, _, body, _ = req(base, "/v1/worlds/montreal-episode/registration", V)
        reg = json.loads(body)
        check("world registration", st == 200 and "authoritative_binding" in reg, body[:300])
        st, _, _, _ = req(base, "/v1/worlds/montreal-episode", S)
        check("world hidden from stranger", st == 404, st)
stop(proc)

# Bounds: rate limiting, saturation and timeout, with a deliberately slow backend.
slow = tmp / "slow-mapserv"
slow.write_text("#!/bin/sh\nif [ \"$1\" = -v ]; then echo 'MapServer version 0 SUPPORTS=WMS_SERVER'; exit 0; fi\nsleep 5\n")
os.chmod(slow, 0o700)
cfg2 = make_config(tmp / "state-slow", str(slow), {"burst": 5, "rate_per_second": 1, "queue_timeout_ms": 100}, {"timeout_ms": 600, "max_concurrency": 1})
proc, base = start(cfg2, log)
codes = [req(base, "/v1/catalog", tokens["burst"])[0] for _ in range(8)]
check("rate limit", codes[:5] == [200] * 5 and 429 in codes[5:], codes)
t0 = time.perf_counter()
st, _, body, _ = req(base, "/v1/maps/montreal/wms?" + q2950.replace("WIDTH=512", "WIDTH=500"), ADM)
check("backend timeout bounded", st == 504 and time.perf_counter() - t0 < 3, (st, body[:120]))
with concurrent.futures.ThreadPoolExecutor(3) as pool:
    codes = list(pool.map(lambda i: req(base, "/v1/maps/montreal/wms?" + q2950.replace("WIDTH=512", f"WIDTH={300 + i}"), ADM)[0], range(3)))
check("backend saturation sheds load", 503 in codes and set(codes) <= {503, 504}, codes)
alive = subprocess.run(["pgrep", "-f", str(slow)], capture_output=True, text=True).stdout.strip()
check("timed-out backends are reaped", alive == "", alive)
stop(proc)

# Private token file required.
os.chmod(token_file, 0o644)
r = subprocess.run([a.fvmapd, "check-config", "--config", str(config)], capture_output=True, text=True)
check("token file must be private", r.returncode != 0, r.stderr[-200:])
os.chmod(token_file, 0o600)

log.close()
results["log_lines"] = sum(1 for _ in open(tmp / "fvmapd.log"))
results["log_is_json"] = all(json.loads(line) is not None for line in open(tmp / "fvmapd.log") if line.strip())
check("structured JSON logs", results["log_is_json"], "")
leak = [line for line in open(tmp / "fvmapd.log") if any(t in line for t in tokens.values())]
check("tokens never logged", not leak, len(leak))
results["passed"] = len(results["checks"]) - len(failures)
results["failed"] = failures
if a.out:
    shutil.copy(tmp / "preview-2950.png", Path(a.out).with_suffix(".png"))
    Path(a.out).write_text(json.dumps(results, indent=1))
shutil.rmtree(tmp)
print(f"{results['passed']}/{len(results['checks'])} integration checks passed")
sys.exit(1 if failures else 0)
