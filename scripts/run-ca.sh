#!/usr/bin/env bash
# Start edgehost with the Certificate Authority enabled and open the CA SPA
# in a browser.
#
# Usage (from anywhere):
#   ./scripts/run-ca.sh
#   EDGEHOST_PORT=18081 ./scripts/run-ca.sh
#   OPEN_BROWSER=0 ./scripts/run-ca.sh          # server only
#   APPLY_SCHEMA=0 ./scripts/run-ca.sh          # skip psql schema apply
#   NO_BUILD=1 ./scripts/run-ca.sh
#   FOREGROUND=0 ./scripts/run-ca.sh            # daemonize + print pid
#
# Browser (password: lab):
#   http://127.0.0.1:18080/ca/          → CA admin (login, create CA, sign CSR, CRL)
#   http://127.0.0.1:18080/ca/crl.pem   → public CRL (no auth)
#   http://127.0.0.1:18080/lab/         → lab console
#
# Postgres (Unix socket defaults):
#   CA_PG_SOCK=/var/run/postgresql/.s.PGSQL.5432
#   CA_PG_DATABASE=edgehost
#   CA_PG_USER=edgehost
#   CA_PG_PASSWORD=                     # empty = trust/peer
#
# Schema: sql/postgres/002_ca.sql (applied when APPLY_SCHEMA=1 and psql works)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PORT="${EDGEHOST_PORT:-18080}"
HOST="${EDGEHOST_HOST:-127.0.0.1}"
PASS="${EDGEHOST_LAB_PASSWORD:-lab}"
HMAC="${EDGEHOST_SESSION_HMAC:-dev-hmac-key-32-bytes-minimum!!}"
CFG_SRC="$ROOT/config/edgehost.ca-lab.yaml"
CFG_RUNTIME="${EDGEHOST_CA_RUNTIME_CFG:-$ROOT/var/edgehost.ca-lab.runtime.yaml}"
LOG="${EDGEHOST_LOG:-$ROOT/build/edgehost-ca.log}"
FOREGROUND="${FOREGROUND:-1}"
NO_BUILD="${NO_BUILD:-0}"
OPEN_BROWSER="${OPEN_BROWSER:-1}"
APPLY_SCHEMA="${APPLY_SCHEMA:-1}"
URL_PATH="${EDGEHOST_CA_URL_PATH:-/ca/}"

CA_PG_SOCK="${CA_PG_SOCK:-/var/run/postgresql/.s.PGSQL.5432}"
CA_PG_DATABASE="${CA_PG_DATABASE:-edgehost}"
CA_PG_USER="${CA_PG_USER:-edgehost}"
CA_PG_PASSWORD="${CA_PG_PASSWORD:-}"

die() { echo "error: $*" >&2; exit 1; }

port_listening() {
  local p="$1"
  if command -v ss >/dev/null 2>&1; then
    ss -ltn 2>/dev/null | grep -qE ":${p}\\b" && return 0
  fi
  return 1
}

wait_http() {
  local url="$1"
  local tries="${2:-40}"
  local i
  for i in $(seq 1 "$tries"); do
    if curl -sS -o /dev/null --max-time 1 "$url" 2>/dev/null; then
      return 0
    fi
    sleep 0.15
  done
  return 1
}

open_browser() {
  local url="$1"
  echo "==> opening browser: $url"
  if command -v xdg-open >/dev/null 2>&1; then
    xdg-open "$url" >/dev/null 2>&1 || true
  elif command -v sensible-browser >/dev/null 2>&1; then
    sensible-browser "$url" >/dev/null 2>&1 || true
  elif command -v gio >/dev/null 2>&1; then
    gio open "$url" >/dev/null 2>&1 || true
  elif command -v open >/dev/null 2>&1; then
    open "$url" >/dev/null 2>&1 || true
  elif command -v wslview >/dev/null 2>&1; then
    wslview "$url" >/dev/null 2>&1 || true
  else
    echo "    (no xdg-open/open found — open the URL manually)"
  fi
}

write_runtime_cfg() {
  local src="$1"
  local dst="$2"
  mkdir -p "$(dirname "$dst")"
  # Overlay CA socket/database/user (and optional password) for lab overrides.
  awk -v sock="$CA_PG_SOCK" -v db="$CA_PG_DATABASE" -v user="$CA_PG_USER" \
      -v pass="$CA_PG_PASSWORD" '
    BEGIN { in_ca = 0 }
    /^  ca:/ { in_ca = 1 }
    in_ca && /^  [a-zA-Z_]/ {
      if ($0 !~ /^  ca:/) in_ca = 0
    }
    in_ca && /^[[:space:]]+pg_sock:[[:space:]]*/ {
      print "    pg_sock: " sock
      next
    }
    in_ca && /^[[:space:]]+database:[[:space:]]*/ {
      print "    database: " db
      next
    }
    in_ca && /^[[:space:]]+user:[[:space:]]*/ {
      print "    user: " user
      next
    }
    in_ca && /^[[:space:]]+# password:/ {
      if (pass != "") {
        print "    password: \"" pass "\""
        next
      }
    }
    { print }
  ' "$src" >"$dst"
  [[ -s "$dst" ]] || die "failed to write runtime config $dst"
}

apply_schema() {
  local schema="$ROOT/sql/postgres/002_ca.sql"
  local sock_dir sock_base
  [[ -f "$schema" ]] || die "missing $schema"

  if ! command -v psql >/dev/null 2>&1; then
    echo "==> skip schema: psql not found (install postgresql-client)"
    echo "    apply later: psql -h /var/run/postgresql -d ${CA_PG_DATABASE} -f sql/postgres/002_ca.sql"
    return 0
  fi

  sock_dir="$(dirname "$CA_PG_SOCK")"
  sock_base="$(basename "$CA_PG_SOCK")"
  # libpq -h is the directory; port from .s.PGSQL.NNNN if present
  local pg_host="$sock_dir"
  local pg_port="5432"
  if [[ "$sock_base" =~ \.s\.PGSQL\.([0-9]+)$ ]]; then
    pg_port="${BASH_REMATCH[1]}"
  fi

  echo "==> applying CA schema via Unix socket"
  echo "    host dir: $pg_host  port: $pg_port  db: $CA_PG_DATABASE  user: $CA_PG_USER"
  if [[ -n "$CA_PG_PASSWORD" ]]; then
    export PGPASSWORD="$CA_PG_PASSWORD"
  fi
  if psql -h "$pg_host" -p "$pg_port" -U "$CA_PG_USER" -d "$CA_PG_DATABASE" \
      -v ON_ERROR_STOP=1 -f "$schema" >/tmp/edgehost-ca-schema.log 2>&1; then
    echo "    schema ok"
  else
    echo "warning: schema apply failed (see /tmp/edgehost-ca-schema.log)"
    echo "         CA UI will start; create-CA / sign need a working DB + schema."
    tail -n 15 /tmp/edgehost-ca-schema.log 2>/dev/null || true
  fi
}

echo "==> edgehost Certificate Authority (browser lab)"
[[ -f "$CFG_SRC" ]] || die "missing $CFG_SRC"
[[ -d "$ROOT/spa/ca" ]] || die "missing spa/ca (CA UI)"

if [[ ! -x "$ROOT/build/edgehost" ]]; then
  if [[ "$NO_BUILD" == "1" ]]; then
    die "build/edgehost missing and NO_BUILD=1"
  fi
  echo "==> building edgehost"
  cmake -B build -S . >/dev/null
  cmake --build build -j"$(nproc 2>/dev/null || echo 2)"
fi
[[ -x "$ROOT/build/edgehost" ]] || die "build/edgehost missing"

write_runtime_cfg "$CFG_SRC" "$CFG_RUNTIME"
CFG="$CFG_RUNTIME"

if [[ "$APPLY_SCHEMA" == "1" ]]; then
  apply_schema
else
  echo "==> skip schema (APPLY_SCHEMA=0)"
fi

export EDGEHOST_LAB_PASSWORD="$PASS"
export EDGEHOST_SESSION_HMAC="$HMAC"

if port_listening "$PORT"; then
  echo "warning: something already listening on port ${PORT}"
  echo "         set EDGEHOST_PORT or stop the other process"
fi

CA_URL="http://${HOST}:${PORT}${URL_PATH}"
CRL_URL="http://${HOST}:${PORT}/ca/crl.pem"

echo "==> starting edgehost on http://${HOST}:${PORT}/"
echo "    config: $CFG"
echo "    log:    $LOG"
echo "    login password: ${PASS}"
echo ""
echo "  CA UI:  ${CA_URL}"
echo "  CRL:    ${CRL_URL}"
echo "  Home:   http://${HOST}:${PORT}/"
echo "  Lab:    http://${HOST}:${PORT}/lab/"
echo ""

mkdir -p "$(dirname "$LOG")"

if [[ "$FOREGROUND" == "1" ]]; then
  # Background long enough to wait for /health and open the browser, then
  # reattach logs to the foreground process.
  "$ROOT/build/edgehost" --config "$CFG" --host "$HOST" --port "$PORT" \
    >"$LOG" 2>&1 &
  EH_PID=$!
  trap 'kill "$EH_PID" 2>/dev/null || true' EXIT INT TERM

  if wait_http "http://${HOST}:${PORT}/health" 50; then
    echo "==> edgehost ready (pid $EH_PID)"
  else
    echo "error: edgehost did not become ready; last log lines:" >&2
    tail -n 30 "$LOG" >&2 || true
    exit 1
  fi

  if [[ "$OPEN_BROWSER" == "1" ]]; then
    open_browser "$CA_URL"
  else
    echo "==> OPEN_BROWSER=0 — open ${CA_URL} yourself"
  fi

  echo "==> foreground (Ctrl-C stops edgehost); logging to $LOG"
  # Drop EXIT trap kill only after we decide to wait; keep INT/TERM
  trap 'kill "$EH_PID" 2>/dev/null || true; exit 130' INT TERM
  # Follow log until process exits
  tail -n +1 -f "$LOG" --pid="$EH_PID" 2>/dev/null || wait "$EH_PID"
  wait "$EH_PID" 2>/dev/null || true
  trap - EXIT INT TERM
else
  nohup "$ROOT/build/edgehost" --config "$CFG" --host "$HOST" --port "$PORT" \
    >"$LOG" 2>&1 &
  EH_PID=$!
  echo "edgehost pid $EH_PID (FOREGROUND=0)"
  if wait_http "http://${HOST}:${PORT}/health" 50; then
    echo "    ready"
    if [[ "$OPEN_BROWSER" == "1" ]]; then
      open_browser "$CA_URL"
    fi
  else
    echo "warning: health check timed out — see $LOG"
  fi
  echo "  stop: kill $EH_PID"
fi
