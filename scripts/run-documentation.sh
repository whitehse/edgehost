#!/usr/bin/env bash
# Link libanim documentation lesson + player assets, start edgehost.
#
#   ./scripts/run-documentation.sh
#   http://127.0.0.1:18080/documentation/
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PORT="${EDGEHOST_PORT:-18080}"
HOST="${EDGEHOST_HOST:-127.0.0.1}"
PASS="${EDGEHOST_LAB_PASSWORD:-lab}"
HMAC="${EDGEHOST_SESSION_HMAC:-dev-hmac-key-32-bytes-minimum!!}"
ANIM="${LIBANIM_ROOT:-$HOME/libanim}"
DOC_DIR="$ROOT/spa/documentation"
EXPLAIN_DIR="$ROOT/spa/explain"
CFG="$ROOT/config/edgehost.status-map.yaml"
LOG="${EDGEHOST_LOG:-$ROOT/build/edgehost-documentation.log}"
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

echo "==> edgehost documentation (2-port tap lesson)"
[[ -d "$ANIM" ]] || die "libanim not found at $ANIM"
[[ -f "$CFG" ]] || die "missing $CFG"
[[ -d "$DOC_DIR" ]] || die "missing $DOC_DIR"

if [[ "$NO_BUILD" != "1" ]]; then
  echo "==> build libanim native + wasm"
  cmake -B "$ANIM/build" -S "$ANIM"
  cmake --build "$ANIM/build"
  ctest --test-dir "$ANIM/build" --output-on-failure || true
  rm -rf "$ANIM/build-wasm"
  cmake -B "$ANIM/build-wasm" -S "$ANIM" \
    -DCMAKE_TOOLCHAIN_FILE="$ANIM/cmake/WasmToolchain.cmake" \
    -DANIM_BUILD_WASM=ON
  cmake --build "$ANIM/build-wasm"
fi
[[ -f "$ANIM/demo/anim.wasm" ]] || die "missing anim.wasm"
[[ -f "$ANIM/fixtures/two_port_tap.anim" ]] || die "missing two_port_tap.anim"

echo "==> link player into spa/explain/player (shared with /explain/)"
mkdir -p "$EXPLAIN_DIR/player" "$EXPLAIN_DIR/templates" "$DOC_DIR/lessons"
link_or_refresh "$ANIM/demo/anim.wasm" "$EXPLAIN_DIR/player/anim.wasm"
link_or_refresh "$ANIM/demo/wasm_host.js" "$EXPLAIN_DIR/player/wasm_host.js"
link_or_refresh "$ANIM/demo/webgpu_renderer.js" "$EXPLAIN_DIR/player/webgpu_renderer.js"
link_or_refresh "$ANIM/demo/demo.css" "$EXPLAIN_DIR/player/demo.css"
link_or_refresh "$ANIM/demo/demo.js" "$EXPLAIN_DIR/player/demo.js"
link_or_refresh "$ANIM/fixtures/two_port_tap.anim" "$DOC_DIR/lessons/two_port_tap.anim"
link_or_refresh "$ANIM/fixtures/templates/two_port_tap.tmpl" "$EXPLAIN_DIR/templates/two_port_tap.tmpl"
# keep other templates if present
for f in optical_path outage_story; do
  [[ -f "$ANIM/fixtures/templates/${f}.tmpl" ]] && \
    link_or_refresh "$ANIM/fixtures/templates/${f}.tmpl" "$EXPLAIN_DIR/templates/${f}.tmpl"
done

if [[ "$NO_BUILD" != "1" ]]; then
  echo "==> build edgehost"
  cmake -B "$ROOT/build" -S "$ROOT" -DLIBANIM_ROOT="$ANIM"
  cmake --build "$ROOT/build" -j"$(nproc 2>/dev/null || echo 2)"
fi
[[ -x "$ROOT/build/edgehost" ]] || die "build/edgehost missing"

export EDGEHOST_LAB_PASSWORD="$PASS"
export EDGEHOST_SESSION_HMAC="$HMAC"

echo "==> start edgehost ${HOST}:${PORT}"
echo "    open http://${HOST}:${PORT}/documentation/"
echo "    log  $LOG"

if [[ "$FOREGROUND" == "1" ]]; then
  exec "$ROOT/build/edgehost" --config "$CFG" --host "$HOST" --port "$PORT" 2>&1 | tee "$LOG"
else
  nohup "$ROOT/build/edgehost" --config "$CFG" --host "$HOST" --port "$PORT" >"$LOG" 2>&1 &
  echo "pid $!"
fi
