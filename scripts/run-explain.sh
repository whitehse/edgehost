#!/usr/bin/env bash
# Link libanim demo + templates into spa/explain and start edgehost.
#
# Usage (from edgehost repo root):
#   ./scripts/run-explain.sh
#   EDGEHOST_PORT=18080 ./scripts/run-explain.sh
#
# Browser:
#   http://127.0.0.1:18080/explain/  (login password: lab)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PORT="${EDGEHOST_PORT:-18080}"
HOST="${EDGEHOST_HOST:-127.0.0.1}"
PASS="${EDGEHOST_LAB_PASSWORD:-lab}"
HMAC="${EDGEHOST_SESSION_HMAC:-dev-hmac-key-32-bytes-minimum!!}"
ANIM="${LIBANIM_ROOT:-$HOME/libanim}"
EXPLAIN_DIR="$ROOT/spa/explain"
CFG="$ROOT/config/edgehost.status-map.yaml"
LOG="${EDGEHOST_LOG:-$ROOT/build/edgehost-explain.log}"
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
  ln -sfn "$target" "$linkpath"
  echo "  linked $linkpath → $target"
}

echo "==> edgehost fiber explain"
echo "    libanim: $ANIM"
echo "    spa:     $EXPLAIN_DIR"

[[ -d "$ANIM" ]] || die "libanim not found at $ANIM (set LIBANIM_ROOT)"
[[ -f "$CFG" ]] || die "missing $CFG"
[[ -d "$EXPLAIN_DIR" ]] || die "missing $EXPLAIN_DIR"

echo "==> ensure libanim native + wasm"
if [[ "$NO_BUILD" != "1" ]]; then
  cmake -B "$ANIM/build" -S "$ANIM"
  cmake --build "$ANIM/build"
  cmake -B "$ANIM/build-wasm" -S "$ANIM" \
    -DCMAKE_TOOLCHAIN_FILE="$ANIM/cmake/WasmToolchain.cmake" \
    -DANIM_BUILD_WASM=ON
  cmake --build "$ANIM/build-wasm"
fi
[[ -f "$ANIM/demo/anim.wasm" ]] || die "missing $ANIM/demo/anim.wasm — build wasm"

echo "==> linking player + templates into spa/explain/"
mkdir -p "$EXPLAIN_DIR/player" "$EXPLAIN_DIR/templates" "$EXPLAIN_DIR/fixtures"
link_or_refresh "$ANIM/demo/demo.js" "$EXPLAIN_DIR/player/demo.js"
link_or_refresh "$ANIM/demo/demo.css" "$EXPLAIN_DIR/player/demo.css"
link_or_refresh "$ANIM/demo/wasm_host.js" "$EXPLAIN_DIR/player/wasm_host.js"
link_or_refresh "$ANIM/demo/webgpu_renderer.js" "$EXPLAIN_DIR/player/webgpu_renderer.js"
link_or_refresh "$ANIM/demo/anim.wasm" "$EXPLAIN_DIR/player/anim.wasm"
link_or_refresh "$ANIM/demo/index.html" "$EXPLAIN_DIR/player/index.html"
# templates for /api/v1/explain (server-side fill)
if [[ -d "$ANIM/fixtures/templates" ]]; then
  for f in "$ANIM/fixtures/templates"/*.tmpl; do
    [[ -f "$f" ]] || continue
    base="$(basename "$f")"
    link_or_refresh "$f" "$EXPLAIN_DIR/templates/$base"
  done
fi
link_or_refresh "$ANIM/fixtures/optical_path.anim" "$EXPLAIN_DIR/fixtures/optical_path.anim"
link_or_refresh "$ANIM/fixtures/outage_story.anim" "$EXPLAIN_DIR/fixtures/outage_story.anim"
link_or_refresh "$ANIM/fixtures/two_port_tap.anim" "$EXPLAIN_DIR/fixtures/two_port_tap.anim"
mkdir -p "$ROOT/spa/documentation/lessons"
link_or_refresh "$ANIM/fixtures/two_port_tap.anim" "$ROOT/spa/documentation/lessons/two_port_tap.anim"

if [[ ! -x "$ROOT/build/edgehost" ]]; then
  if [[ "$NO_BUILD" == "1" ]]; then
    die "build/edgehost missing and NO_BUILD=1"
  fi
  echo "==> building edgehost (with libanim if present)"
  cmake -B "$ROOT/build" -S "$ROOT" -DLIBANIM_ROOT="$ANIM"
  cmake --build "$ROOT/build" -j"$(nproc 2>/dev/null || echo 2)"
else
  # rebuild to pick up explain routes / libanim
  if [[ "$NO_BUILD" != "1" ]]; then
    cmake -B "$ROOT/build" -S "$ROOT" -DLIBANIM_ROOT="$ANIM"
    cmake --build "$ROOT/build" -j"$(nproc 2>/dev/null || echo 2)"
  fi
fi

export EDGEHOST_LAB_PASSWORD="$PASS"
export EDGEHOST_SESSION_HMAC="$HMAC"

echo "==> starting edgehost on ${HOST}:${PORT}"
echo "    log: $LOG"
echo "    open: http://${HOST}:${PORT}/explain/"

if [[ "$FOREGROUND" == "1" ]]; then
  exec "$ROOT/build/edgehost" --config "$CFG" --host "$HOST" --port "$PORT" 2>&1 | tee "$LOG"
else
  nohup "$ROOT/build/edgehost" --config "$CFG" --host "$HOST" --port "$PORT" >"$LOG" 2>&1 &
  echo "pid $!"
fi
