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

if ! $all && grep -qE '^(\.github/workflows/ci\.yml|tools/ci-affected\.sh)$' "$TMP/changed"; then
    all=true
    echo "ci-affected: CI configuration changed; running everything" >&2
fi

makefile=false
if ! $all && grep -qx 'Makefile' "$TMP/changed"; then
    makefile=true
    if git diff -U0 $RANGE -- Makefile | grep -E '^[+-]' | grep -vE '^(\+\+\+|---)' | grep -qE 'unit_|test-[a-z]|TEST_UNITS|FS_CORE_SRC|LENS_SRC|g\+\+'; then
        all=true
        echo "ci-affected: the Makefile's host test rules changed; running every unit" >&2
    else
        echo "ci-affected: Makefile changed outside the host test rules; units unchanged" >&2
    fi
fi

picked=""
for u in $UNITS; do
    if $all; then
        picked="$picked $u"
        continue
    fi
    make -s unit-deps UNIT="$u" > "$TMP/deps"
    if [ "$u" = apxpack ]; then find userland -type f >> "$TMP/deps"; fi
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
elif $makefile || grep -qE '^(kernel/|limine/|userland/|tools/(ci-boot|ci-shell|ci-usb|crash-fuzz)\.sh$|tools/(bakefont\.py|stellarfs\.cpp)$)' "$TMP/changed"; then
    boot=true
    echo "ci-affected: boot  <- $(grep -E '^(kernel/|limine/|tools/)' "$TMP/changed" | head -n 1)" >&2
else
    echo "ci-affected: boot  unchanged" >&2
fi

FS_ONLY='^(kernel/src/fs/|kernel/include/cosmos/(stellar|nebula|nebula_path|nebula_realms|lens|lens_channel|lens_text|lens_types)\.hpp$|tools/stellarfs\.cpp$)'
BOOT_RELEVANT='^(kernel/|limine/|userland/|tools/(ci-boot|ci-shell|ci-usb|crash-fuzz)\.sh$|tools/(bakefont\.py|stellarfs\.cpp)$)'
drivers=true
if ! $all && ! $makefile && $boot; then
    if ! grep -E "$BOOT_RELEVANT" "$TMP/changed" | grep -qvE "$FS_ONLY"; then
        drivers=false
        echo "ci-affected: drivers  unchanged (only filesystem files changed: smoke boot on nvme only)" >&2
    fi
fi

if $drivers; then
    boot_matrix='[{"disk":"nvme","cores":1},{"disk":"nvme","cores":4},{"disk":"ahci","cores":1},{"disk":"virtio","cores":1}]'
    fuzz_matrix='[{"disk":"nvme","rounds":16},{"disk":"ahci","rounds":10},{"disk":"virtio","rounds":10}]'
else
    boot_matrix='[{"disk":"nvme","cores":1}]'
    fuzz_matrix='[{"disk":"nvme","rounds":16}]'
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
echo "boot_matrix=$boot_matrix"
echo "fuzz_matrix=$fuzz_matrix"
