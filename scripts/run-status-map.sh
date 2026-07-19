#!/usr/bin/env bash
# Link libwebmap demo map assets into spa/map and start edgehost for the
# company login + status map UI.
#
# Usage (from edgehost repo root, or any cwd):
#   ./scripts/run-status-map.sh
#   EDGEHOST_PORT=18080 ./scripts/run-status-map.sh
#   LIBWEBMAP_DEMO=/path/to/libwebmap/demo ./scripts/run-status-map.sh
#   NO_BUILD=1 ./scripts/run-status-map.sh          # skip cmake if binary exists
#   FOREGROUND=0 ./scripts/run-status-map.sh        # daemonize + print pid
#
# Browser:
#   http://127.0.0.1:18080/     → login (password: lab)
#   http://127.0.0.1:18080/map/ → status map (after login)
#   http://127.0.0.1:18080/lab/ → API lab console
#
# Requires WebGPU-capable browser (Chrome/Edge). First basemap tiles may take
# a few seconds; fiber path_index is large (~20 MiB) and loads on demand.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PORT="${EDGEHOST_PORT:-18080}"
HOST="${EDGEHOST_HOST:-127.0.0.1}"
PASS="${EDGEHOST_LAB_PASSWORD:-lab}"
HMAC="${EDGEHOST_SESSION_HMAC:-dev-hmac-key-32-bytes-minimum!!}"
DEMO="${LIBWEBMAP_DEMO:-$HOME/libwebmap/demo}"
MAP_DIR="$ROOT/spa/map"
CFG="$ROOT/config/edgehost.status-map.yaml"
LOG="${EDGEHOST_LOG:-$ROOT/build/edgehost-status-map.log}"
FOREGROUND="${FOREGROUND:-1}"
NO_BUILD="${NO_BUILD:-0}"

die() { echo "error: $*" >&2; exit 1; }

link_or_refresh() {
  local target="$1"
  local linkpath="$2"
  if [[ ! -e "$target" && ! -L "$target" ]]; then
    echo "  skip (missing): $target"
    return 0
  fi
  if [[ -L "$linkpath" || -e "$linkpath" ]]; then
    rm -rf "$linkpath"
  fi
  ln -s "$target" "$linkpath"
  echo "  linked $linkpath → $target"
}

echo "==> edgehost status map"
echo "    demo: $DEMO"
echo "    map:  $MAP_DIR"

[[ -d "$DEMO" ]] || die "libwebmap demo not found at $DEMO (set LIBWEBMAP_DEMO)"
[[ -f "$CFG" ]] || die "missing $CFG"
[[ -d "$MAP_DIR" ]] || die "missing $MAP_DIR"

echo "==> linking demo assets into spa/map/"
# Core host + styles
link_or_refresh "$DEMO/main.js" "$MAP_DIR/main.js"
link_or_refresh "$DEMO/display" "$MAP_DIR/display"
link_or_refresh "$DEMO/webmap.wasm" "$MAP_DIR/webmap.wasm"
# Map packages (large — not copied into git)
link_or_refresh "$DEMO/basemap" "$MAP_DIR/basemap"
link_or_refresh "$DEMO/fiber_data" "$MAP_DIR/fiber_data"
link_or_refresh "$DEMO/tiles_fiber" "$MAP_DIR/tiles_fiber"
# Optional demo extras (weather / dynamic feed fixtures)
[[ -d "$DEMO/weather" ]] && link_or_refresh "$DEMO/weather" "$MAP_DIR/weather"
[[ -d "$DEMO/dynamic" ]] && link_or_refresh "$DEMO/dynamic" "$MAP_DIR/dynamic"
[[ -d "$DEMO/splice_diagrams" ]] && link_or_refresh "$DEMO/splice_diagrams" "$MAP_DIR/splice_diagrams"

# Catalog entry for ops (packages remain auth-gated; map data is under SPA for now)
if [[ -f "$ROOT/packages/index.json" ]]; then
  echo "==> packages/index.json present (fixture catalog; map tiles served under /map/)"
fi

if [[ ! -x "$ROOT/build/edgehost" ]]; then
  if [[ "$NO_BUILD" == "1" ]]; then
    die "build/edgehost missing and NO_BUILD=1"
  fi
  echo "==> building edgehost"
  cmake -B build -S . >/dev/null
  cmake --build build -j"$(nproc 2>/dev/null || echo 2)"
fi
[[ -x "$ROOT/build/edgehost" ]] || die "build/edgehost missing"

export EDGEHOST_LAB_PASSWORD="$PASS"
export EDGEHOST_SESSION_HMAC="$HMAC"

# Avoid clobbering an already-bound port without a clear message
if command -v ss >/dev/null 2>&1; then
  if ss -ltn 2>/dev/null | grep -q ":${PORT} "; then
    echo "warning: something already listening on ${HOST}:${PORT}"
    echo "         set EDGEHOST_PORT or stop the other process"
  fi
fi

echo "==> starting edgehost on http://${HOST}:${PORT}/"
echo "    config: $CFG"
echo "    log:    $LOG"
echo "    login password: ${PASS}"
echo ""
echo "  Open:  http://${HOST}:${PORT}/"
echo "  Map:   http://${HOST}:${PORT}/map/"
echo "  Lab:   http://${HOST}:${PORT}/lab/"
echo ""

if [[ "$FOREGROUND" == "1" ]]; then
  exec "$ROOT/build/edgehost" --config "$CFG" --host "$HOST" --port "$PORT" 2>&1 | tee "$LOG"
else
  nohup "$ROOT/build/edgehost" --config "$CFG" --host "$HOST" --port "$PORT" \
    >"$LOG" 2>&1 &
  echo "edgehost pid $! (FOREGROUND=0)"
  echo "  stop: kill $!"
fi
