#!/usr/bin/env bash
# Offline CPU flame graph via perf + FlameGraph scripts (when installed).
# Prefer lab console POST /api/v1/debug/cpu/profile for in-process SIGPROF.
#
# Usage:
#   scripts/cpu-flame-perf.sh [pid] [seconds]
#   scripts/cpu-flame-perf.sh $(pidof edgehost) 30
#
# Requires: perf, stackcollapse-perf.pl, flamegraph.pl (Brendan Gregg FlameGraph)
# Kernel: lower /proc/sys/kernel/perf_event_paranoid if needed (often <= 1).
set -euo pipefail

PID="${1:-}"
SECS="${2:-20}"
OUTDIR="${OUTDIR:-/tmp/edgehost-flame}"
mkdir -p "$OUTDIR"

if [[ -z "$PID" ]]; then
  PID=$(pidof edgehost | awk '{print $1}')
fi
if [[ -z "$PID" ]]; then
  echo "usage: $0 <pid> [seconds]" >&2
  exit 1
fi

if ! command -v perf >/dev/null 2>&1; then
  echo "perf not installed (apt install linux-perf)" >&2
  exit 1
fi

DATA="$OUTDIR/perf-${PID}.data"
FOLDED="$OUTDIR/out.folded"
SVG="$OUTDIR/flame-${PID}.svg"

echo "recording pid=$PID for ${SECS}s → $DATA"
perf record -F 99 -g -p "$PID" -- sleep "$SECS"
perf script -i "$DATA" > "$OUTDIR/out.perf"

if command -v stackcollapse-perf.pl >/dev/null 2>&1; then
  stackcollapse-perf.pl "$OUTDIR/out.perf" > "$FOLDED"
elif [[ -x "$HOME/FlameGraph/stackcollapse-perf.pl" ]]; then
  "$HOME/FlameGraph/stackcollapse-perf.pl" "$OUTDIR/out.perf" > "$FOLDED"
else
  echo "stackcollapse-perf.pl not found; left $OUTDIR/out.perf" >&2
  echo "Clone https://github.com/brendangregg/FlameGraph and re-run." >&2
  exit 0
fi

if command -v flamegraph.pl >/dev/null 2>&1; then
  flamegraph.pl "$FOLDED" > "$SVG"
elif [[ -x "$HOME/FlameGraph/flamegraph.pl" ]]; then
  "$HOME/FlameGraph/flamegraph.pl" "$FOLDED" > "$SVG"
else
  echo "folded stacks: $FOLDED (install flamegraph.pl to render SVG)" >&2
  exit 0
fi

echo "wrote $SVG"
echo "also folded: $FOLDED"
