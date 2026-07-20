#!/usr/bin/env bash
# Link libwebmap demo map assets and start edgehost with multi-vendor Call Home:
#   - Calix E7 (identity XML + <ack>ok</ack> + SSH client + exa-events)
#   - Juniper Junos (DEVICE-CONN-INFO + SSH client + NETCONF stream)
#   - SPAs: /junos/ (add Junos systems), /e7/ (full admin), /map/
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
#   FOREGROUND=0 NO_BUILD=1 ./scripts/run-status-map-junos.sh
#
# Aliases: E7_HOST/E7_PORT → CH_HOST/CH_PORT
#
# Browser (password: lab):
#   http://127.0.0.1:18080/junos/  → add Junos DEVICE-ID + optional secret
#   http://127.0.0.1:18080/e7/     → multi-vendor Call Home admin
#   http://127.0.0.1:18080/map/    → status map
#
# Guide: docs/guides/e7-callhome.md
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
MAP_DIR="$ROOT/spa/map"
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

echo "==> edgehost status map + multi-vendor Call Home (Calix + Junos)"
echo "    demo:      $DEMO"
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

echo "==> linking demo assets into spa/map/"
link_or_refresh "$DEMO/main.js" "$MAP_DIR/main.js"
link_or_refresh "$DEMO/display" "$MAP_DIR/display"
link_or_refresh "$DEMO/webmap.wasm" "$MAP_DIR/webmap.wasm"
link_or_refresh "$DEMO/basemap" "$MAP_DIR/basemap"
link_or_refresh "$DEMO/fiber_data" "$MAP_DIR/fiber_data"
link_or_refresh "$DEMO/tiles_fiber" "$MAP_DIR/tiles_fiber"
[[ -d "$DEMO/weather" ]] && link_or_refresh "$DEMO/weather" "$MAP_DIR/weather"
[[ -d "$DEMO/dynamic" ]] && link_or_refresh "$DEMO/dynamic" "$MAP_DIR/dynamic"
[[ -d "$DEMO/splice_diagrams" ]] && link_or_refresh "$DEMO/splice_diagrams" "$MAP_DIR/splice_diagrams"

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
  cmake -B build -S . >/dev/null
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

echo "==> starting edgehost (multi-vendor Call Home)"
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
echo "  Home:      http://${HTTP_URL_HOST}:${PORT}/"
echo "  Map:       http://${HTTP_URL_HOST}:${PORT}/map/"
echo "  Junos UI:  http://${HTTP_URL_HOST}:${PORT}/junos/  (add DEVICE-ID systems)"
echo "  Full CH:   http://${HTTP_URL_HOST}:${PORT}/e7/     (Calix + Junos admin)"
echo "  Lab:       http://${HTTP_URL_HOST}:${PORT}/lab/"
echo "  Call Home: ${CH_HOST}:${CH_PORT}"
echo ""
echo "  Calix E7s and Junos routers dial the same port; identity preamble selects path."
echo "  Add Calix MACs and Junos DEVICE-IDs to the shared allowlist (SPA or file)."
echo ""

if [[ "$FOREGROUND" == "1" ]]; then
  exec "$ROOT/build/edgehost" --config "$CFG_RUNTIME" --host "$HOST" --port "$PORT" 2>&1 | tee "$LOG"
else
  nohup "$ROOT/build/edgehost" --config "$CFG_RUNTIME" --host "$HOST" --port "$PORT" \
    >"$LOG" 2>&1 &
  echo "edgehost pid $! (FOREGROUND=0)"
  echo "  stop: kill $!"
fi
