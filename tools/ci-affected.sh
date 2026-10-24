#!/bin/sh
set -eu

RANGE="${1:-}"
cd "$(dirname "$0")/.."
UNITS="$(make -s print-units)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

all=false
if [ "${CI_RUN_ALL:-0}" = 1 ] || [ -z "$RANGE" ]; then
    all=true
    echo "ci-affected: running everything (no range, or CI_RUN_ALL=1)" >&2
elif ! git diff --name-only $RANGE -- > "$TMP/changed" 2>/dev/null; then
    all=true
    echo "ci-affected: cannot diff '$RANGE'; running everything" >&2
fi

if ! $all && grep -qE '^(Makefile|\.github/workflows/ci\.yml|tools/ci-affected\.sh)$' "$TMP/changed"; then
    all=true
    echo "ci-affected: build or CI configuration changed; running everything" >&2
fi

picked=""
for u in $UNITS; do
    if $all; then
        picked="$picked $u"
        continue
    fi
    make -s unit-deps UNIT="$u" > "$TMP/deps"
    hit="$(grep -Fx -f "$TMP/deps" "$TMP/changed" | head -n 1 || true)"
    if [ -n "$hit" ]; then
        picked="$picked $u"
        echo "ci-affected: $u  <- $hit" >&2
    else
        echo "ci-affected: $u  unchanged" >&2
    fi
done

boot=false
if $all; then
    boot=true
elif grep -qE '^(kernel/|limine/|tools/(ci-boot|ci-shell|ci-usb|crash-fuzz)\.sh$|tools/(bakefont\.py|stellarfs\.cpp)$)' "$TMP/changed"; then
    boot=true
    echo "ci-affected: boot  <- $(grep -E '^(kernel/|limine/|tools/)' "$TMP/changed" | head -n 1)" >&2
else
    echo "ci-affected: boot  unchanged" >&2
fi

json="["
sep=""
for u in $picked; do
    json="$json$sep\"$u\""
    sep=","
done
json="$json]"
echo "units=$json"
echo "boot=$boot"
