#!/usr/bin/env bash
# Link libwebmap demo map assets into spa/map and start edgehost with both:
#   - company login + WebGPU status map (/map/)
#   - Calix E7 Call Home configure + monitor SPA (/e7/)
#
# Usage (from edgehost repo root, or any cwd):
#   ./scripts/run-status-map-e7.sh
#   EDGEHOST_PORT=18080 E7_PORT=4334 ./scripts/run-status-map-e7.sh
#   E7_HOST=0.0.0.0 E7_PORT=4334 ./scripts/run-status-map-e7.sh
#   E7_HOST=192.0.2.10 ./scripts/run-status-map-e7.sh   # bind Call Home only on one IP
#   LIBWEBMAP_DEMO=/path/to/libwebmap/demo ./scripts/run-status-map-e7.sh
#   NO_BUILD=1 ./scripts/run-status-map-e7.sh          # skip cmake if binary exists
#   FOREGROUND=0 ./scripts/run-status-map-e7.sh        # daemonize + print pid
#
# Browser (password: lab):
#   http://127.0.0.1:18080/     → home (login → map by default)
#   http://127.0.0.1:18080/map/ → status map
#   http://127.0.0.1:18080/e7/  → E7 Call Home status, shelves, ONTs, commands
#   http://127.0.0.1:18080/lab/ → API lab console
#
# Call Home (default lab YAML uses transport: raw — cleartext after identity):
#   Default bind: 0.0.0.0:4334 (all interfaces — field E7s can dial in).
#   Override: E7_HOST / E7_PORT (rewritten into a runtime YAML; not CLI flags).
#   Peer: identity (MAC) → NETCONF client hello (raw). For SSH Call Home set
#   transport: ssh + ssh_username/password in the YAML (needs libassh).
#   REST: /api/v1/e7/*  · allowlist file: ./var/e7_allowlist.txt
#
# Map requires WebGPU (Chrome/Edge). Fiber path_index is large (~20 MiB).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PORT="${EDGEHOST_PORT:-18080}"
# HTTP SPA/API bind (CLI --host/--port). Keep loopback by default for the UI.
HOST="${EDGEHOST_HOST:-127.0.0.1}"
# Call Home agent bind (plugins.e7_callhome.listen_*). All interfaces by default.
E7_HOST="${E7_HOST:-0.0.0.0}"
E7_PORT="${E7_PORT:-4334}"
PASS="${EDGEHOST_LAB_PASSWORD:-lab}"
HMAC="${EDGEHOST_SESSION_HMAC:-dev-hmac-key-32-bytes-minimum!!}"
DEMO="${LIBWEBMAP_DEMO:-$HOME/libwebmap/demo}"
MAP_DIR="$ROOT/spa/map"
CFG_SRC="$ROOT/config/edgehost.status-map-e7.yaml"
CFG_RUNTIME="${EDGEHOST_E7_RUNTIME_CFG:-$ROOT/var/edgehost.status-map-e7.runtime.yaml}"
LOG="${EDGEHOST_LOG:-$ROOT/build/edgehost-status-map-e7.log}"
FOREGROUND="${FOREGROUND:-1}"
NO_BUILD="${NO_BUILD:-0}"
VAR_DIR="$ROOT/var"
ALLOWLIST="$VAR_DIR/e7_allowlist.txt"

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

port_listening() {
  local p="$1"
  if command -v ss >/dev/null 2>&1; then
    ss -ltn 2>/dev/null | grep -q ":${p} " && return 0
  fi
  return 1
}

# Write runtime YAML with E7_HOST / E7_PORT applied (edgehost has no --e7-host CLI).
write_runtime_cfg() {
  local src="$1"
  local dst="$2"
  local e7h="$3"
  local e7p="$4"
  mkdir -p "$(dirname "$dst")"
  # Replace only the e7_callhome listen_* scalars (first match after e7 block is fine
  # for this template). Preserve rest of the profile (SSH creds, SPA, state, …).
  awk -v e7h="$e7h" -v e7p="$e7p" '
    BEGIN { in_e7 = 0 }
    /^  e7_callhome:/ { in_e7 = 1 }
    in_e7 && /^  [a-zA-Z]/ { if ($0 !~ /^  e7_callhome:/) in_e7 = 0 }
    in_e7 && /^[[:space:]]+listen_host:[[:space:]]*/ {
      print "    listen_host: " e7h
      next
    }
    in_e7 && /^[[:space:]]+listen_port:[[:space:]]*/ {
      print "    listen_port: " e7p
      next
    }
    { print }
  ' "$src" >"$dst"
  [[ -s "$dst" ]] || die "failed to write runtime config $dst"
}

echo "==> edgehost status map + E7 Call Home"
echo "    demo:    $DEMO"
echo "    map:     $MAP_DIR"
echo "    cfg src: $CFG_SRC"
echo "    E7_HOST: $E7_HOST  E7_PORT: $E7_PORT"

[[ -d "$DEMO" ]] || die "libwebmap demo not found at $DEMO (set LIBWEBMAP_DEMO)"
[[ -f "$CFG_SRC" ]] || die "missing $CFG_SRC"
[[ -d "$MAP_DIR" ]] || die "missing $MAP_DIR"
[[ -n "$E7_HOST" ]] || die "E7_HOST is empty"
[[ "$E7_PORT" =~ ^[0-9]+$ ]] || die "E7_PORT must be a number (got: $E7_PORT)"
if (( E7_PORT < 1 || E7_PORT > 65535 )); then
  die "E7_PORT out of range 1–65535 (got: $E7_PORT)"
fi

echo "==> linking demo assets into spa/map/"
# Core host + styles
link_or_refresh "$DEMO/main.js" "$MAP_DIR/main.js"
link_or_refresh "$DEMO/display" "$MAP_DIR/display"
link_or_refresh "$DEMO/webmap.wasm" "$MAP_DIR/webmap.wasm"
# Map packages (large — not copied into git)
link_or_refresh "$DEMO/basemap" "$MAP_DIR/basemap"
link_or_refresh "$DEMO/fiber_data" "$MAP_DIR/fiber_data"
link_or_refresh "$DEMO/tiles_fiber" "$MAP_DIR/tiles_fiber"
# Optional demo extras
[[ -d "$DEMO/weather" ]] && link_or_refresh "$DEMO/weather" "$MAP_DIR/weather"
[[ -d "$DEMO/dynamic" ]] && link_or_refresh "$DEMO/dynamic" "$MAP_DIR/dynamic"
[[ -d "$DEMO/splice_diagrams" ]] && link_or_refresh "$DEMO/splice_diagrams" "$MAP_DIR/splice_diagrams"

# Durable E7 allowlist file (config allowlist_path); seed empty if absent
mkdir -p "$VAR_DIR"
if [[ ! -f "$ALLOWLIST" ]]; then
  # YAML shelves still seed runtime; file captures REST edits across restarts.
  : >"$ALLOWLIST"
  echo "==> created empty allowlist $ALLOWLIST"
else
  echo "==> allowlist $ALLOWLIST ($(wc -l <"$ALLOWLIST" | tr -d ' ') lines)"
fi

echo "==> writing runtime config (E7 bind ${E7_HOST}:${E7_PORT})"
write_runtime_cfg "$CFG_SRC" "$CFG_RUNTIME" "$E7_HOST" "$E7_PORT"
echo "    $CFG_RUNTIME"
# Show what was applied
grep -E 'listen_host:|listen_port:|transport:|ssh_username:' "$CFG_RUNTIME" | head -10 | sed 's/^/    /'

if [[ -f "$ROOT/packages/index.json" ]]; then
  echo "==> packages/index.json present (fixture catalog; map tiles under /map/)"
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

# Prefer rebuilt libassh with OpenSSH-like banner (Calix rejects SSH-2.0-LIBASSH).
# Built at ~/libassh-1.1 when source is present; falls back to system libassh.
ASSH_LIBDIR="${LIBASSH_LIBDIR:-}"
if [[ -z "$ASSH_LIBDIR" ]]; then
  if [[ -d "$HOME/libassh-1.1/src/.libs" ]]; then
    ASSH_LIBDIR="$HOME/libassh-1.1/src/.libs"
  fi
fi
if [[ -n "$ASSH_LIBDIR" && -e "$ASSH_LIBDIR/libassh.so" ]]; then
  export LD_LIBRARY_PATH="${ASSH_LIBDIR}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  echo "==> libassh from $ASSH_LIBDIR (LD_LIBRARY_PATH)"
  if command -v strings >/dev/null 2>&1; then
    strings "$ASSH_LIBDIR/libassh.so" 2>/dev/null | grep -m1 'SSH-2.0' | sed 's/^/    banner: /' || true
  fi
fi

if port_listening "$PORT"; then
  echo "warning: something already listening on ${HOST}:${PORT}"
  echo "         set EDGEHOST_PORT or stop the other process"
fi
if port_listening "$E7_PORT"; then
  echo "warning: something already listening on Call Home port ${E7_PORT}"
  echo "         set E7_PORT or free the port"
fi

echo "==> starting edgehost"
echo "    HTTP:      ${HOST}:${PORT}  (SPA + REST; EDGEHOST_HOST / EDGEHOST_PORT)"
echo "    Call Home: ${E7_HOST}:${E7_PORT}  (see YAML transport; E7_HOST / E7_PORT)"
echo "    config:    $CFG_RUNTIME"
echo "    log:       $LOG"
echo "    login password: ${PASS}"
echo ""
# Browser URL uses a loopback-friendly display when HTTP is all-interfaces
HTTP_URL_HOST="$HOST"
if [[ "$HOST" == "0.0.0.0" ]]; then
  HTTP_URL_HOST="127.0.0.1"
fi
echo "  Home:     http://${HTTP_URL_HOST}:${PORT}/"
echo "  Map:      http://${HTTP_URL_HOST}:${PORT}/map/"
echo "  E7 admin: http://${HTTP_URL_HOST}:${PORT}/e7/"
echo "  Lab:      http://${HTTP_URL_HOST}:${PORT}/lab/"
echo "  Health:   http://${HTTP_URL_HOST}:${PORT}/health"
echo "  E7 CH:    ${E7_HOST}:${E7_PORT} (SSH Call Home — configure shelves at /e7/)"
echo ""
echo "  After login: open Map for the rural status map, or E7 Call Home to"
echo "  configure shelves (allowlist), watch sessions, ONTs, and commands."
echo ""

if [[ "$FOREGROUND" == "1" ]]; then
  exec "$ROOT/build/edgehost" --config "$CFG_RUNTIME" --host "$HOST" --port "$PORT" 2>&1 | tee "$LOG"
else
  nohup "$ROOT/build/edgehost" --config "$CFG_RUNTIME" --host "$HOST" --port "$PORT" \
    >"$LOG" 2>&1 &
  echo "edgehost pid $! (FOREGROUND=0)"
  echo "  stop: kill $!"
fi
