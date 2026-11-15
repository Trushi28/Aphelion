#!/bin/sh
set -eu

RANGE="${1:-}"
cd "$(dirname "$0")/.."
UNITS="$(make -s print-units)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# full: every unit, every disk driver. Only for a run with no commit range to look
# at (manual or scheduled) and for a change to CI itself.
# all_units: every host unit, but not the driver matrix. A Makefile test-rule edit
# needs the first without the second.
full=false
if [ "${CI_RUN_ALL:-0}" = 1 ] || [ -z "$RANGE" ]; then
    full=true
    echo "ci-affected: running everything (no range, or CI_RUN_ALL=1)" >&2
elif ! git diff --name-only $RANGE -- > "$TMP/changed" 2>/dev/null; then
    full=true
    echo "ci-affected: cannot diff '$RANGE'; running everything" >&2
fi

if ! $full && grep -qE '^(\.github/workflows/ci\.yml|tools/ci-affected\.sh|tools/ci-quiet\.sh)$' "$TMP/changed"; then
    full=true
    echo "ci-affected: CI configuration changed; running everything" >&2
fi

all_units=$full
makefile=false
if ! $full && grep -qx 'Makefile' "$TMP/changed"; then
    makefile=true
    if git diff -U0 $RANGE -- Makefile | grep -E '^[+-]' | grep -vE '^(\+\+\+|---)' | grep -qE 'unit_|test-[a-z]|TEST_UNITS|FS_CORE_SRC|LENS_SRC|g\+\+'; then
        all_units=true
        echo "ci-affected: the Makefile's host test rules changed; running every unit" >&2
    else
        echo "ci-affected: Makefile changed outside the host test rules; units unchanged" >&2
    fi
fi

picked=""
for u in $UNITS; do
    if $all_units; then
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

FS_ONLY='^(kernel/src/fs/|kernel/include/cosmos/(stellar|nebula|nebula_path|nebula_realms|lens|lens_channel|lens_text|lens_types)\.hpp$|tools/stellarfs\.cpp$)'
BOOT_RELEVANT='^(kernel/|limine/|userland/|tools/(ci-boot|ci-shell|ci-usb|crash-fuzz)\.sh$|tools/(bakefont\.py|stellarfs\.cpp)$)'

boot=false
if $full; then
    boot=true
elif $makefile || grep -qE "$BOOT_RELEVANT" "$TMP/changed"; then
    boot=true
    echo "ci-affected: boot  <- $(grep -E '^(kernel/|limine/|tools/)' "$TMP/changed" | head -n 1)" >&2
else
    echo "ci-affected: boot  unchanged" >&2
fi

# The AHCI and VirtIO boots and fuzz runs exist to test the disk drivers, so they run
# only when something a disk driver is built from has changed: the driver sources,
# the PCI and DMA code under them, and every header they include. That list comes
# from the compiler, the same way the host units' dependencies do, so a header a
# driver starts to include is picked up without editing this script.
DRIVER_SRC="kernel/src/drivers/nvme.cpp kernel/src/drivers/ahci.cpp kernel/src/drivers/virtio_blk.cpp kernel/src/drivers/blockdev.cpp kernel/src/arch/x86_64/pci.cpp kernel/src/mm/dma.cpp"
drivers=false
if $full; then
    drivers=true
elif $boot; then
    if g++ -std=c++20 -MM -Ikernel/include $DRIVER_SRC > "$TMP/driver-mm" 2>"$TMP/driver-mm.err"; then
        tr -d '\\' < "$TMP/driver-mm" | tr ' ' '\n' | grep -E '^(kernel|tools)/' | sed -e 's,[^ /]*/\.\./,,g' | sort -u > "$TMP/driver-deps"
        # the scripts that decide how each disk is attached
        printf '%s\n' tools/ci-boot.sh tools/crash-fuzz.sh >> "$TMP/driver-deps"
        hit="$(grep -Fx -f "$TMP/driver-deps" "$TMP/changed" | head -n 1 || true)"
        if [ -n "$hit" ]; then
            drivers=true
            echo "ci-affected: drivers  <- $hit" >&2
        else
            echo "ci-affected: drivers  unchanged (ahci and virtio are not booted)" >&2
        fi
    else
        # Not knowing is not a reason to skip them.
        drivers=true
        echo "ci-affected: cannot work out what the drivers depend on; booting every driver" >&2
        head -n 5 "$TMP/driver-mm.err" >&2 || true
    fi
fi

# Four cores on NVMe is the SMP check, not a driver check. It stays for any kernel
# change except a filesystem-only one, so a scheduler or memory change still gets it.
smp=false
if $full || $drivers; then
    smp=true
elif $boot; then
    if $makefile || grep -E "$BOOT_RELEVANT" "$TMP/changed" | grep -qvE "$FS_ONLY"; then
        smp=true
    fi
fi

# One smoke boot on NVMe always runs when the kernel does: the self-tests need a disk.
boot_matrix='{"disk":"nvme","cores":1}'
fuzz_matrix='{"disk":"nvme","rounds":16}'
if $smp; then
    boot_matrix="$boot_matrix,{\"disk\":\"nvme\",\"cores\":4}"
fi
if $drivers; then
    boot_matrix="$boot_matrix,{\"disk\":\"ahci\",\"cores\":1},{\"disk\":\"virtio\",\"cores\":1}"
    fuzz_matrix="$fuzz_matrix,{\"disk\":\"ahci\",\"rounds\":10},{\"disk\":\"virtio\",\"rounds\":10}"
fi
boot_matrix="[$boot_matrix]"
fuzz_matrix="[$fuzz_matrix]"
if $boot; then
    echo "ci-affected: matrix  boot=$boot_matrix  fuzz=$fuzz_matrix" >&2
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
