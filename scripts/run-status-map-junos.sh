#!/usr/bin/env bash
# Link libwebmap demo map assets and start edgehost with multi-vendor Call Home:
#   - Calix E7 (identity XML + <ack>ok</ack> + SSH client + exa-events)
#   - Juniper Junos (DEVICE-CONN-INFO + SSH client + NETCONF stream)
#   - SPAs: /junos/ (add Junos systems), /e7/ (full admin), /map/
#   - libanim: /documentation/ (2-port tap lesson), /explain/ (template lab)
#
# IMPORTANT — one listener on CH_PORT (default 4334):
#   This script shares the same allowlist and Call Home port as
#   ./scripts/run-status-map-e7.sh. Do not run both at once on the same port.
#   Starting this script with an empty Junos-only allowlist used to reject all
#   Calix dials; that is fixed — we always use ./var/e7_allowlist.txt.
#
# Usage:
#   ./scripts/run-status-map-junos.sh
#   CH_HOST=0.0.0.0 CH_PORT=4334 ./scripts/run-status-map-junos.sh
#   JUNOS_DEVICE_ID=pe1.lab JUNOS_SECRET=shared ./scripts/run-status-map-junos.sh
#   # Only if all devices use this NMS SSH login (defaults keep Calix sysadmin):
#   JUNOS_SSH_USER=netconf JUNOS_SSH_PASSWORD=secret ./scripts/run-status-map-junos.sh
#   LIBANIM_ROOT=$HOME/libanim ./scripts/run-status-map-junos.sh
#   SKIP_LIBANIM=1 ./scripts/run-status-map-junos.sh   # map + Call Home only
#   FOREGROUND=0 NO_BUILD=1 ./scripts/run-status-map-junos.sh
#
# Aliases: E7_HOST/E7_PORT → CH_HOST/CH_PORT
#
# Browser (password: lab):
#   http://127.0.0.1:18080/junos/          → add Junos DEVICE-ID + optional secret
#   http://127.0.0.1:18080/e7/             → multi-vendor Call Home admin
#   http://127.0.0.1:18080/map/            → status map
#   http://127.0.0.1:18080/documentation/  → 2-port tap animation lesson
#   http://127.0.0.1:18080/explain/        → template fill + player lab
#
# Guide: docs/guides/e7-callhome.md
# Tap lesson: libanim docs/guides/two-port-tap.md
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PORT="${EDGEHOST_PORT:-18080}"
HOST="${EDGEHOST_HOST:-127.0.0.1}"
CH_HOST="${CH_HOST:-${E7_HOST:-0.0.0.0}}"
CH_PORT="${CH_PORT:-${E7_PORT:-4334}}"
PASS="${EDGEHOST_LAB_PASSWORD:-lab}"
HMAC="${EDGEHOST_SESSION_HMAC:-dev-hmac-key-32-bytes-minimum!!}"
# Default SSH client user matches Calix field (sysadmin). Only override when
# JUNOS_SSH_USER is set — rewriting to netconf/lab used to break Calix dials.
SSH_USER="${JUNOS_SSH_USER:-${E7_SSH_USER:-sysadmin}}"
SSH_PASS="${JUNOS_SSH_PASSWORD:-${E7_SSH_PASSWORD:-sysadmin}}"
SEED_ID="${JUNOS_DEVICE_ID:-}"
SEED_SECRET="${JUNOS_SECRET:-}"
SEED_LABEL="${JUNOS_LABEL:-junos}"
DEMO="${LIBWEBMAP_DEMO:-$HOME/libwebmap/demo}"
ANIM="${LIBANIM_ROOT:-$HOME/libanim}"
SKIP_LIBANIM="${SKIP_LIBANIM:-0}"
MAP_DIR="$ROOT/spa/map"
EXPLAIN_DIR="$ROOT/spa/explain"
DOC_DIR="$ROOT/spa/documentation"
CFG_SRC="$ROOT/config/edgehost.status-map-junos.yaml"
CFG_RUNTIME="${EDGEHOST_JUNOS_RUNTIME_CFG:-$ROOT/var/edgehost.status-map-junos.runtime.yaml}"
LOG="${EDGEHOST_LOG:-$ROOT/build/edgehost-status-map-junos.log}"
FOREGROUND="${FOREGROUND:-1}"
NO_BUILD="${NO_BUILD:-0}"
VAR_DIR="$ROOT/var"
# SHARED with Calix E7 — never use a separate empty junos-only file here.
ALLOWLIST="$VAR_DIR/e7_allowlist.txt"
LEGACY_JUNOS_ALLOWLIST="$VAR_DIR/junos_allowlist.txt"

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

port_listening() {
  local p="$1"
  if command -v ss >/dev/null 2>&1; then
    ss -ltn 2>/dev/null | grep -q ":${p} " && return 0
  fi
  return 1
}

write_runtime_cfg() {
  local src="$1"
  local dst="$2"
  local ch_h="$3"
  local ch_p="$4"
  local ssh_u="$5"
  local ssh_p="$6"
  mkdir -p "$(dirname "$dst")"
  awk -v ch_h="$ch_h" -v ch_p="$ch_p" -v ssh_u="$ssh_u" -v ssh_p="$ssh_p" '
    BEGIN { in_e7 = 0 }
    /^  e7_callhome:/ { in_e7 = 1 }
    in_e7 && /^  [a-zA-Z]/ { if ($0 !~ /^  e7_callhome:/) in_e7 = 0 }
    in_e7 && /^[[:space:]]+listen_host:[[:space:]]*/ {
      print "    listen_host: " ch_h
      next
    }
    in_e7 && /^[[:space:]]+listen_port:[[:space:]]*/ {
      print "    listen_port: " ch_p
      next
    }
    in_e7 && /^[[:space:]]+ssh_username:[[:space:]]*/ {
      print "    ssh_username: " ssh_u
      next
    }
    in_e7 && /^[[:space:]]+ssh_password:[[:space:]]*/ {
      if (ssh_p ~ /[:#{}[\],&*!|>'"'"'%@`]/ || ssh_p ~ / /) {
        gsub(/"/, "\\\"", ssh_p)
        print "    ssh_password: \"" ssh_p "\""
      } else {
        print "    ssh_password: " ssh_p
      }
      next
    }
    { print }
  ' "$src" >"$dst"
  [[ -s "$dst" ]] || die "failed to write runtime config $dst"
}

# Merge any legacy junos-only allowlist lines into the shared e7 allowlist once.
migrate_legacy_junos_allowlist() {
  if [[ ! -f "$LEGACY_JUNOS_ALLOWLIST" ]]; then
    return 0
  fi
  local n
  n=$(grep -cE 'device_id=|vendor=junos' "$LEGACY_JUNOS_ALLOWLIST" 2>/dev/null || true)
  if [[ "${n:-0}" -eq 0 ]]; then
    return 0
  fi
  echo "==> migrating Junos lines from $LEGACY_JUNOS_ALLOWLIST → $ALLOWLIST"
  mkdir -p "$VAR_DIR"
  if [[ ! -f "$ALLOWLIST" ]]; then
    echo "# edgehost e7 allowlist v1 (shared Calix + Junos)" >"$ALLOWLIST"
  fi
  while IFS= read -r line || [[ -n "$line" ]]; do
    [[ -z "$line" || "$line" =~ ^# ]] && continue
    if [[ "$line" =~ device_id= || "$line" =~ vendor=junos ]]; then
      local did
      did=$(echo "$line" | sed -n 's/.*device_id=\([^ ]*\).*/\1/p')
      if [[ -n "$did" ]] && grep -qE "device_id=${did}([[:space:]]|$)" "$ALLOWLIST" 2>/dev/null; then
        continue
      fi
      echo "$line" >>"$ALLOWLIST"
      echo "    + $line"
    fi
  done <"$LEGACY_JUNOS_ALLOWLIST"
}

ensure_allowlist() {
  mkdir -p "$VAR_DIR"
  if [[ ! -f "$ALLOWLIST" ]]; then
    {
      echo "# edgehost e7 allowlist v1 (shared Calix MAC + Junos DEVICE-ID)"
      echo "# mac=… vendor=calix | device_id=… vendor=junos [secret=…]"
    } >"$ALLOWLIST"
    echo "==> created shared allowlist $ALLOWLIST"
  else
    echo "==> shared allowlist $ALLOWLIST ($(wc -l <"$ALLOWLIST" | tr -d ' ') lines)"
  fi
  migrate_legacy_junos_allowlist

  if [[ -z "$SEED_ID" ]]; then
    return 0
  fi
  if grep -E "(^|[[:space:]])device_id=${SEED_ID}([[:space:]]|$)" "$ALLOWLIST" >/dev/null 2>&1 \
     || grep -E "^mac=${SEED_ID}([[:space:]]|$)" "$ALLOWLIST" >/dev/null 2>&1; then
    echo "==> JUNOS_DEVICE_ID=${SEED_ID} already in allowlist"
    return 0
  fi
  local line="device_id=${SEED_ID} vendor=junos enabled=1 label=${SEED_LABEL}"
  if [[ -n "$SEED_SECRET" ]]; then
    line+=" secret=${SEED_SECRET}"
  fi
  echo "$line" >>"$ALLOWLIST"
  echo "==> seeded Junos device_id=${SEED_ID}${SEED_SECRET:+ (secret set)} into shared allowlist"
}

link_map_assets() {
  echo "==> linking libwebmap demo assets into spa/map/"
  link_or_refresh "$DEMO/main.js" "$MAP_DIR/main.js"
  link_or_refresh "$DEMO/display" "$MAP_DIR/display"
  link_or_refresh "$DEMO/webmap.wasm" "$MAP_DIR/webmap.wasm"
  link_or_refresh "$DEMO/basemap" "$MAP_DIR/basemap"
  link_or_refresh "$DEMO/fiber_data" "$MAP_DIR/fiber_data"
  link_or_refresh "$DEMO/tiles_fiber" "$MAP_DIR/tiles_fiber"
  [[ -d "$DEMO/weather" ]] && link_or_refresh "$DEMO/weather" "$MAP_DIR/weather"
  [[ -d "$DEMO/dynamic" ]] && link_or_refresh "$DEMO/dynamic" "$MAP_DIR/dynamic"
  [[ -d "$DEMO/splice_diagrams" ]] && link_or_refresh "$DEMO/splice_diagrams" "$MAP_DIR/splice_diagrams"
}

# libanim player + templates + 2-port tap documentation lesson
link_libanim_assets() {
  if [[ "$SKIP_LIBANIM" == "1" ]]; then
    echo "==> SKIP_LIBANIM=1 — not linking documentation / explain assets"
    return 0
  fi
  if [[ ! -d "$ANIM" ]]; then
    echo "warning: libanim not found at $ANIM (set LIBANIM_ROOT) — skip animation SPA links"
    return 0
  fi

  echo "==> linking libanim assets (documentation + explain)"
  echo "    libanim: $ANIM"

  if [[ "$NO_BUILD" != "1" ]]; then
    if [[ ! -f "$ANIM/demo/anim.wasm" ]]; then
      echo "==> building libanim freestanding WASM (anim.wasm missing)"
      cmake -B "$ANIM/build-wasm" -S "$ANIM" \
        -DCMAKE_TOOLCHAIN_FILE="$ANIM/cmake/WasmToolchain.cmake" \
        -DANIM_BUILD_WASM=ON
      cmake --build "$ANIM/build-wasm"
    fi
    # Prefer rebuilding native lib when edgehost will link it
    if [[ ! -f "$ANIM/build/libanim.a" && ! -f "$ANIM/build/libanim.so" ]]; then
      echo "==> building libanim native library"
      cmake -B "$ANIM/build" -S "$ANIM"
      cmake --build "$ANIM/build"
    fi
  fi

  if [[ ! -f "$ANIM/demo/anim.wasm" ]]; then
    echo "warning: missing $ANIM/demo/anim.wasm — documentation player will not load WASM"
  fi

  mkdir -p "$EXPLAIN_DIR/player" "$EXPLAIN_DIR/templates" "$EXPLAIN_DIR/fixtures" \
           "$DOC_DIR/lessons"

  link_or_refresh "$ANIM/demo/demo.js" "$EXPLAIN_DIR/player/demo.js"
  link_or_refresh "$ANIM/demo/demo.css" "$EXPLAIN_DIR/player/demo.css"
  link_or_refresh "$ANIM/demo/wasm_host.js" "$EXPLAIN_DIR/player/wasm_host.js"
  link_or_refresh "$ANIM/demo/webgpu_renderer.js" "$EXPLAIN_DIR/player/webgpu_renderer.js"
  link_or_refresh "$ANIM/demo/anim.wasm" "$EXPLAIN_DIR/player/anim.wasm"
  link_or_refresh "$ANIM/demo/index.html" "$EXPLAIN_DIR/player/index.html"
  # Voice narrator is first-party SPA code under spa/explain/player/narrator.js
  # (not from libanim demo). Ensure file is present for documentation imports.
  if [[ ! -f "$EXPLAIN_DIR/player/narrator.js" ]]; then
    echo "  note: spa/explain/player/narrator.js ships in-tree (Web Speech TTS)"
  fi

  if [[ -d "$ANIM/fixtures/templates" ]]; then
    for f in "$ANIM/fixtures/templates"/*.tmpl; do
      [[ -f "$f" ]] || continue
      link_or_refresh "$f" "$EXPLAIN_DIR/templates/$(basename "$f")"
    done
  fi
  for name in optical_path outage_story two_port_tap; do
    [[ -f "$ANIM/fixtures/${name}.anim" ]] && \
      link_or_refresh "$ANIM/fixtures/${name}.anim" "$EXPLAIN_DIR/fixtures/${name}.anim"
  done
  # Documentation screen lesson (2-port tap)
  if [[ -f "$ANIM/fixtures/two_port_tap.anim" ]]; then
    link_or_refresh "$ANIM/fixtures/two_port_tap.anim" "$DOC_DIR/lessons/two_port_tap.anim"
  else
    echo "warning: missing fixtures/two_port_tap.anim — /documentation/ lesson unavailable"
  fi
}

echo "==> edgehost status map + multi-vendor Call Home (Calix + Junos) + libanim docs"
echo "    demo:      $DEMO"
echo "    libanim:   $ANIM  (SKIP_LIBANIM=$SKIP_LIBANIM)"
echo "    cfg src:   $CFG_SRC"
echo "    CH_HOST:   $CH_HOST  CH_PORT: $CH_PORT"
echo "    SSH user:  $SSH_USER  (default sysadmin for Calix; set JUNOS_SSH_USER only if needed)"
echo "    allowlist: $ALLOWLIST  (shared — Calix MACs + Junos DEVICE-IDs)"

[[ -d "$DEMO" ]] || die "libwebmap demo not found at $DEMO (set LIBWEBMAP_DEMO)"
[[ -f "$CFG_SRC" ]] || die "missing $CFG_SRC"
[[ -d "$MAP_DIR" ]] || die "missing $MAP_DIR"
[[ -n "$CH_HOST" ]] || die "CH_HOST is empty"
[[ "$CH_PORT" =~ ^[0-9]+$ ]] || die "CH_PORT must be a number (got: $CH_PORT)"
if (( CH_PORT < 1 || CH_PORT > 65535 )); then
  die "CH_PORT out of range 1–65535 (got: $CH_PORT)"
fi

link_map_assets
link_libanim_assets

ensure_allowlist

echo "==> writing runtime config (Call Home bind ${CH_HOST}:${CH_PORT})"
write_runtime_cfg "$CFG_SRC" "$CFG_RUNTIME" "$CH_HOST" "$CH_PORT" "$SSH_USER" "$SSH_PASS"
echo "    $CFG_RUNTIME"
grep -E 'listen_host:|listen_port:|transport:|ssh_username:|subscription_stream:|allowlist_path:' \
  "$CFG_RUNTIME" | head -12 | sed 's/^/    /'
if grep -q 'ssh_password:' "$CFG_RUNTIME"; then
  echo "    ssh_password: (set, hidden)"
fi
# Safety: refuse empty-looking exclusive allowlist path
if grep -q 'junos_allowlist' "$CFG_RUNTIME"; then
  die "runtime config still points at junos_allowlist — must use shared e7_allowlist.txt"
fi

if [[ ! -x "$ROOT/build/edgehost" ]]; then
  if [[ "$NO_BUILD" == "1" ]]; then
    die "build/edgehost missing and NO_BUILD=1"
  fi
  echo "==> building edgehost"
  if [[ "$SKIP_LIBANIM" != "1" && -d "$ANIM" ]]; then
    cmake -B build -S . -DLIBANIM_ROOT="$ANIM"
  else
    cmake -B build -S .
  fi
  cmake --build build -j"$(nproc 2>/dev/null || echo 2)"
elif [[ "$NO_BUILD" != "1" ]]; then
  # Rebuild so explain/documentation routes and libanim stay in sync
  echo "==> rebuilding edgehost"
  if [[ "$SKIP_LIBANIM" != "1" && -d "$ANIM" ]]; then
    cmake -B build -S . -DLIBANIM_ROOT="$ANIM" >/dev/null
  else
    cmake -B build -S . >/dev/null
  fi
  cmake --build build -j"$(nproc 2>/dev/null || echo 2)"
fi
[[ -x "$ROOT/build/edgehost" ]] || die "build/edgehost missing"

export EDGEHOST_LAB_PASSWORD="$PASS"
export EDGEHOST_SESSION_HMAC="$HMAC"

ASSH_LIBDIR="${LIBASSH_LIBDIR:-}"
if [[ -z "$ASSH_LIBDIR" && -d "$HOME/libassh-1.1/src/.libs" ]]; then
  ASSH_LIBDIR="$HOME/libassh-1.1/src/.libs"
fi
if [[ -n "$ASSH_LIBDIR" && -e "$ASSH_LIBDIR/libassh.so" ]]; then
  export LD_LIBRARY_PATH="${ASSH_LIBDIR}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  echo "==> libassh from $ASSH_LIBDIR"
fi

if port_listening "$PORT"; then
  echo "warning: something already listening on ${HOST}:${PORT}"
fi
if port_listening "$CH_PORT"; then
  echo "warning: something already listening on Call Home port ${CH_PORT}"
  echo "         stop the other edgehost (Calix/Junos share this port)"
fi

echo "==> starting edgehost (multi-vendor Call Home + documentation)"
echo "    HTTP:      ${HOST}:${PORT}"
echo "    Call Home: ${CH_HOST}:${CH_PORT}  (Calix + Junos demux by identity)"
echo "    config:    $CFG_RUNTIME"
echo "    allowlist: $ALLOWLIST"
echo "    log:       $LOG"
echo "    login password: ${PASS}"
echo ""
HTTP_URL_HOST="$HOST"
if [[ "$HOST" == "0.0.0.0" ]]; then
  HTTP_URL_HOST="127.0.0.1"
fi
echo "  Home:           http://${HTTP_URL_HOST}:${PORT}/"
echo "  Map:            http://${HTTP_URL_HOST}:${PORT}/map/"
echo "  Documentation:  http://${HTTP_URL_HOST}:${PORT}/documentation/  (2-port tap lesson)"
echo "  Explain lab:    http://${HTTP_URL_HOST}:${PORT}/explain/"
echo "  Junos UI:       http://${HTTP_URL_HOST}:${PORT}/junos/  (add DEVICE-ID systems)"
echo "  Full CH:        http://${HTTP_URL_HOST}:${PORT}/e7/     (Calix + Junos admin)"
echo "  Lab:            http://${HTTP_URL_HOST}:${PORT}/lab/"
echo "  Call Home:      ${CH_HOST}:${CH_PORT}"
echo ""
echo "  Calix E7s and Junos routers dial the same port; identity preamble selects path."
echo "  Add Calix MACs and Junos DEVICE-IDs to the shared allowlist (SPA or file)."
echo "  Skip animations: SKIP_LIBANIM=1"
echo ""

if [[ "$FOREGROUND" == "1" ]]; then
  exec "$ROOT/build/edgehost" --config "$CFG_RUNTIME" --host "$HOST" --port "$PORT" 2>&1 | tee "$LOG"
else
  nohup "$ROOT/build/edgehost" --config "$CFG_RUNTIME" --host "$HOST" --port "$PORT" \
    >"$LOG" 2>&1 &
  echo "edgehost pid $! (FOREGROUND=0)"
  echo "  stop: kill $!"
fi
