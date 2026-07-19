#!/usr/bin/env bash
# Compare local sibling checkouts to deps/pins.txt.
# Exit 0 if all present pins match (or are absent with --allow-missing).
# Exit 1 on SHA mismatch. Exit 2 on usage error.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SIBLING_ROOT="${SIBLING_ROOT:-${HOME}}"
PINS="$ROOT/deps/pins.txt"
ALLOW_MISSING=0
STRICT_MISSING=0

usage() {
  echo "Usage: deps/verify_pins.sh [--allow-missing] [--strict-missing]"
  exit 2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --allow-missing) ALLOW_MISSING=1 ;;
    --strict-missing) STRICT_MISSING=1 ;;
    -h|--help) usage ;;
    *) usage ;;
  esac
  shift
done

if [[ ! -f "$PINS" ]]; then
  echo "FAIL: missing $PINS" >&2
  exit 1
fi

mismatches=0
missing=0
checked=0

while read -r name sha rest; do
  [[ -z "${name:-}" || "$name" == \#* ]] && continue
  [[ -z "${sha:-}" || "$sha" == \#* ]] && continue
  if [[ "$sha" == \<* || "$sha" == pending* ]]; then
    echo "skip  $name (unpinned)"
    continue
  fi
  path="$SIBLING_ROOT/$name"
  if [[ ! -d "$path/.git" ]]; then
    echo "MISS  $name (no checkout at $path)"
    missing=$((missing + 1))
    continue
  fi
  local_sha="$(git -C "$path" rev-parse HEAD)"
  checked=$((checked + 1))
  if [[ "$local_sha" == "$sha" ]]; then
    echo "ok    $name $sha"
  else
    echo "DIFF  $name pin=$sha local=$local_sha" >&2
    mismatches=$((mismatches + 1))
  fi
done < <(grep -v '^\s*#' "$PINS" | grep -v '^\s*$' || true)

echo "---"
echo "checked=$checked mismatches=$mismatches missing=$missing"

if [[ $mismatches -gt 0 ]]; then
  exit 1
fi
if [[ $STRICT_MISSING -eq 1 && $missing -gt 0 ]]; then
  exit 1
fi
if [[ $ALLOW_MISSING -eq 0 && $missing -gt 0 && $checked -eq 0 ]]; then
  exit 1
fi
exit 0
