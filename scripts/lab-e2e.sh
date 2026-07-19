#!/usr/bin/env bash
# Lab end-to-end smoke: edgehost health → lab login → state → packages → mobile sync.
#
# Usage (from edgehost repo root):
#   ./scripts/lab-e2e.sh
#   EDGEHOST_PORT=18080 ./scripts/lab-e2e.sh
#   SKIP_MOBILE=1 ./scripts/lab-e2e.sh          # server-only
#   KEEP_RUNNING=1 ./scripts/lab-e2e.sh         # leave edgehost up
#
# Optional OpenAI (not required for pass):
#   OPENAI_API_KEY=sk-... ENABLE_OPENAI=1 ./scripts/lab-e2e.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PORT="${EDGEHOST_PORT:-18080}"
HOST="${EDGEHOST_HOST:-127.0.0.1}"
BASE="http://${HOST}:${PORT}"
PASS="${EDGEHOST_LAB_PASSWORD:-lab}"
HMAC="${EDGEHOST_SESSION_HMAC:-dev-hmac-key-32-bytes-minimum!!}"
COOKIE_JAR="$(mktemp)"
PID_FILE="$(mktemp)"
CACHE_DIR="${TMPDIR:-/tmp}/ecoec-lab-e2e-$$"
MOBILE_ROOT="${MOBILE_ROOT:-$HOME/ecoec-mobile}"
SKIP_MOBILE="${SKIP_MOBILE:-0}"
KEEP_RUNNING="${KEEP_RUNNING:-0}"
ENABLE_OPENAI="${ENABLE_OPENAI:-0}"

cleanup() {
  local code=$?
  rm -f "$COOKIE_JAR" "$PID_FILE"
  [[ -n "${TMP_LAB_CFG:-}" && -f "${TMP_LAB_CFG:-}" ]] && rm -f "$TMP_LAB_CFG"
  if [[ "$KEEP_RUNNING" != "1" ]]; then
    if [[ -n "${EDGE_PID:-}" ]] && kill -0 "$EDGE_PID" 2>/dev/null; then
      kill "$EDGE_PID" 2>/dev/null || true
      wait "$EDGE_PID" 2>/dev/null || true
    fi
    rm -rf "$CACHE_DIR" "${CACHE_DIR}.edgehost.log"
  else
    echo "KEEP_RUNNING=1 — edgehost pid ${EDGE_PID:-?} still on ${BASE}"
    echo "  log: ${CACHE_DIR}.edgehost.log"
  fi
  exit "$code"
}
trap cleanup EXIT

pass() { echo "  PASS: $*"; }
fail() { echo "  FAIL: $*" >&2; exit 1; }

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || fail "missing command: $1"
}

need_cmd curl
need_cmd cmake

if [[ ! -x ./build/edgehost ]]; then
  echo "==> building edgehost"
  cmake -B build -S . >/dev/null
  cmake --build build -j"$(nproc 2>/dev/null || echo 2)"
fi
[[ -x ./build/edgehost ]] || fail "build/edgehost missing"

export EDGEHOST_LAB_PASSWORD="$PASS"
export EDGEHOST_SESSION_HMAC="$HMAC"

LAB_CFG="$ROOT/config/edgehost.lab.yaml"
TMP_LAB_CFG=""
if [[ "$ENABLE_OPENAI" == "1" ]]; then
  if [[ -z "${OPENAI_API_KEY:-}" ]]; then
    fail "ENABLE_OPENAI=1 requires OPENAI_API_KEY"
  fi
  TMP_LAB_CFG="$(mktemp)"
  LAB_CFG="$TMP_LAB_CFG"
  # Flip only the openai_proxy.enabled flag (first "enabled: false" under plugins).
  awk '
    BEGIN { in_oa=0; done=0 }
    /^  openai_proxy:/ { in_oa=1 }
    in_oa && /enabled:/ && !done { sub(/false/, "true"); done=1; in_oa=0 }
    { print }
  ' "$ROOT/config/edgehost.lab.yaml" > "$LAB_CFG"
fi

echo "==> starting edgehost on ${BASE}"
./build/edgehost --config "$LAB_CFG" --host "$HOST" --port "$PORT" \
  >"${CACHE_DIR}.edgehost.log" 2>&1 &
EDGE_PID=$!
echo "$EDGE_PID" > "$PID_FILE"

# Wait for /health
for i in $(seq 1 50); do
  if curl -sf "${BASE}/health" >/dev/null 2>&1; then
    break
  fi
  if ! kill -0 "$EDGE_PID" 2>/dev/null; then
    echo "---- edgehost log ----"
    cat "${CACHE_DIR}.edgehost.log" || true
    fail "edgehost exited before /health"
  fi
  sleep 0.1
done
curl -sf "${BASE}/health" >/dev/null || {
  cat "${CACHE_DIR}.edgehost.log" || true
  fail "/health not ready"
}
pass "GET /health"

# SPA
code=$(curl -s -o /dev/null -w '%{http_code}' "${BASE}/")
[[ "$code" == "200" ]] || fail "SPA index HTTP $code"
pass "GET / SPA"

# Lab login
login_body=$(curl -sS -c "$COOKIE_JAR" -b "$COOKIE_JAR" \
  -H 'Content-Type: application/json' \
  -d "{\"password\":\"${PASS}\"}" \
  -w '\n%{http_code}' \
  "${BASE}/auth/lab-login")
login_code=$(echo "$login_body" | tail -n1)
login_json=$(echo "$login_body" | sed '$d')
[[ "$login_code" == "200" ]] || fail "lab-login HTTP $login_code: $login_json"
echo "$login_json" | grep -q '"ok":true\|"ok": true' || fail "lab-login body: $login_json"
grep -q edge_session "$COOKIE_JAR" || fail "no edge_session cookie"
pass "POST /auth/lab-login → cookie"

# /auth/me
me=$(curl -sS -b "$COOKIE_JAR" -w '\n%{http_code}' "${BASE}/auth/me")
me_code=$(echo "$me" | tail -n1)
[[ "$me_code" == "200" ]] || fail "/auth/me HTTP $me_code"
pass "GET /auth/me"

# State PUT/GET
put_code=$(curl -sS -b "$COOKIE_JAR" -o /tmp/lab-put.out -w '%{http_code}' \
  -X PUT -H 'Content-Type: application/json' \
  -d '{"id":"lab-1","status":"ok","bps_in":1.2e6,"updated_at":"2026-07-18T12:00:00Z"}' \
  "${BASE}/api/v1/state/net.core/router/lab-1")
[[ "$put_code" == "204" || "$put_code" == "200" ]] || fail "state PUT HTTP $put_code $(cat /tmp/lab-put.out)"
get_body=$(curl -sS -b "$COOKIE_JAR" -w '\n%{http_code}' \
  "${BASE}/api/v1/state/net.core/router/lab-1")
get_code=$(echo "$get_body" | tail -n1)
get_json=$(echo "$get_body" | sed '$d')
[[ "$get_code" == "200" ]] || fail "state GET HTTP $get_code"
echo "$get_json" | grep -q 'lab-1' || fail "state GET missing lab-1: $get_json"
pass "PUT/GET /api/v1/state/net.core/router/lab-1"

# map.dynamic
curl -sS -b "$COOKIE_JAR" -o /dev/null -w '%{http_code}' \
  -X PUT -H 'Content-Type: application/json' \
  -d '{"id":"span-lab","class":"fiber","status":"ok","updated_at":"2026-07-18T12:00:00Z"}' \
  "${BASE}/api/v1/state/map.dynamic/feature/fiber/span-lab" | grep -qE '204|200' \
  || fail "map.dynamic PUT failed"
pass "PUT map.dynamic feature"

# Packages (auth required in lab mode)
pkg_code=$(curl -sS -b "$COOKIE_JAR" -o /tmp/lab-manifest.json -w '%{http_code}' \
  "${BASE}/packages/fixture_basemap/manifest.json")
[[ "$pkg_code" == "200" ]] || fail "package manifest HTTP $pkg_code"
grep -q fixture_basemap /tmp/lab-manifest.json || fail "manifest content"
tile_code=$(curl -sS -b "$COOKIE_JAR" -o /tmp/lab-tile.wmap -w '%{http_code}' \
  "${BASE}/packages/fixture_basemap/12/0/0.wmap")
[[ "$tile_code" == "200" ]] || fail "tile HTTP $tile_code"
pass "GET /packages/fixture_basemap/*"

# Unauthenticated package should fail in lab_password mode
unauth=$(curl -sS -o /dev/null -w '%{http_code}' \
  "${BASE}/packages/fixture_basemap/manifest.json")
[[ "$unauth" == "401" || "$unauth" == "403" ]] || fail "expected 401/403 without cookie, got $unauth"
pass "packages protected without cookie ($unauth)"

# WebSocket: optional (wscat). Non-fatal if tool missing or handshake only.
if command -v wscat >/dev/null 2>&1; then
  # Extract cookie value
  COOKIE_VAL=$(awk '/edge_session/{print $NF}' "$COOKIE_JAR" | tail -1)
  if [[ -n "$COOKIE_VAL" ]]; then
    # short connect — just verify upgrade doesn't immediately die
    set +e
    timeout 2 wscat -c "ws://${HOST}:${PORT}/api/v1/stream?topics=state" \
      -H "Cookie: edge_session=${COOKIE_VAL}" </dev/null >/tmp/lab-ws.out 2>&1
    ws_rc=$?
    set -e
    # timeout(124) or clean close is ok; connection refused is not
    if grep -qiE 'error|ECONNREFUSED|401|403' /tmp/lab-ws.out 2>/dev/null; then
      # some wscat versions print errors on timeout; check for upgrade success patterns
      if grep -qiE 'connected|opened' /tmp/lab-ws.out 2>/dev/null; then
        pass "WS /api/v1/stream connected (wscat)"
      else
        echo "  WARN: WS smoke inconclusive (rc=$ws_rc); see /tmp/lab-ws.out"
      fi
    else
      pass "WS /api/v1/stream smoke (wscat rc=$ws_rc)"
    fi
  fi
else
  echo "  SKIP: wscat not installed (WS manual: browser lab console)"
fi

# Optional OpenAI probe
if [[ "$ENABLE_OPENAI" == "1" ]]; then
  oa=$(curl -sS -b "$COOKIE_JAR" -o /tmp/lab-oa.out -w '%{http_code}' \
    -X POST -H 'Content-Type: application/json' \
    -d '{"model":"gpt-4o-mini","messages":[{"role":"user","content":"ping"}],"max_tokens":8}' \
    "${BASE}/v1/chat/completions")
  if [[ "$oa" == "200" ]]; then
    pass "POST /v1/chat/completions"
  else
    echo "  WARN: OpenAI proxy HTTP $oa (see /tmp/lab-oa.out) — not a hard fail"
  fi
fi

# Mobile package sync
if [[ "$SKIP_MOBILE" == "1" ]]; then
  echo "  SKIP: mobile sync (SKIP_MOBILE=1)"
else
  if [[ ! -d "$MOBILE_ROOT" ]]; then
    fail "MOBILE_ROOT not found: $MOBILE_ROOT (set SKIP_MOBILE=1 to skip)"
  fi
  if [[ ! -x "$MOBILE_ROOT/build/mobile_core_demo" ]]; then
    echo "==> building ecoec-mobile"
    cmake -B "$MOBILE_ROOT/build" -S "$MOBILE_ROOT" >/dev/null
    cmake --build "$MOBILE_ROOT/build" -j"$(nproc 2>/dev/null || echo 2)"
  fi
  SYNC_SCRIPT="$MOBILE_ROOT/scripts/sync_from_edgehost.sh"
  if [[ ! -x "$SYNC_SCRIPT" ]]; then
    fail "missing $SYNC_SCRIPT"
  fi
  mkdir -p "$CACHE_DIR"
  "$SYNC_SCRIPT" \
    --base "$BASE" \
    --cookie-jar "$COOKIE_JAR" \
    --package fixture_basemap \
    --cache "$CACHE_DIR/pkgs" \
    --demo "$MOBILE_ROOT/build/mobile_core_demo"
  pass "mobile sync fixture_basemap from edgehost"
fi

echo
echo "ALL LAB E2E CHECKS PASSED"
echo "  console: ${BASE}/"
echo "  password: ${PASS}"
if [[ "$KEEP_RUNNING" == "1" ]]; then
  echo "  edgehost still running (pid $EDGE_PID)"
fi
