#!/bin/sh
set -eu

DISK_KIND="${1:-nvme}"
CORES="${2:-1}"
ISO="${ISO:-build/aphelion.iso}"
LOG="$(mktemp)"
IMG="$(mktemp)"

qemu-img create -f raw "$IMG" 64M >/dev/null

case "$DISK_KIND" in
    nvme)   DISK="-drive file=$IMG,if=none,id=d0,format=raw -device nvme,drive=d0,serial=aphelion0" ;;
    ahci)   DISK="-drive file=$IMG,if=none,id=d0,format=raw -device ide-hd,drive=d0,bus=ide.0" ;;
    virtio) DISK="-drive file=$IMG,if=none,id=d0,format=raw -device virtio-blk-pci,drive=d0" ;;
    *) echo "unknown disk kind: $DISK_KIND" >&2; exit 2 ;;
esac

timeout 40 qemu-system-x86_64 -M q35 -cpu max -m 256M -smp "$CORES" \
    -cdrom "$ISO" $DISK -serial "file:$LOG" -display none -no-reboot >/dev/null 2>&1 || true

fail() {
    echo "FAIL ($DISK_KIND, $CORES core(s)): $1"
    tr -d '\r' < "$LOG" | grep -v '^\[demo\]' | tail -40
    exit 1
}

tr -d '\r' < "$LOG" > "$LOG.clean"
grep -q 'Aphelion is up' "$LOG.clean" || fail "kernel did not finish booting"
grep -q 'UNHANDLED INTERRUPT' "$LOG.clean" && fail "unhandled interrupt"
grep -q 'FATAL' "$LOG.clean" && fail "fatal error"
grep -q 'MISMATCH' "$LOG.clean" && fail "self-test mismatch"
grep -q 'snapshot() cost 1 write(s) + 0 read(s)' "$LOG.clean" || fail "snapshot is not O(1)"
grep -q 'stress: 14 stars created (create_ok=1), 0 mismatch(es)' "$LOG.clean" || fail "filesystem stress test failed"

if [ "$CORES" -gt 1 ]; then
    grep -q 'stealing confirmed' "$LOG.clean" || fail "no cross-core work-stealing observed"
fi

echo "ok: $DISK_KIND, $CORES core(s)"
