#!/bin/sh
set -eu

DISK_KIND="${1:-nvme}"
CORES="${2:-1}"
ISO="${ISO:-build/aphelion.iso}"
LIMIT="${BOOT_TIMEOUT:-120}"
LOG_DIR="${LOG_DIR:-build/ci-logs}"
PASS="fresh"
LOG=""

mkdir -p "$LOG_DIR"
IMG="$(mktemp)"
FIMG="$(mktemp)"
trap 'rm -f "$IMG" "$FIMG"' EXIT

qemu-img create -f raw "$IMG" 64M >/dev/null

disk_args() {
    case "$DISK_KIND" in
        nvme)   echo "-drive file=$1,if=none,id=d0,format=raw -device nvme,drive=d0,serial=aphelion0" ;;
        ahci)   echo "-drive file=$1,if=none,id=d0,format=raw -device ide-hd,drive=d0,bus=ide.0" ;;
        virtio) echo "-drive file=$1,if=none,id=d0,format=raw -device virtio-blk-pci,drive=d0" ;;
        *) echo "unknown disk kind: $DISK_KIND" >&2; exit 2 ;;
    esac
}
DISK="$(disk_args "$IMG")"

fail() {
    echo "FAIL ($DISK_KIND, $CORES core(s), $PASS boot): $1"
    grep -v '^\[demo\]' "$LOG.clean" | tail -40
    exit 1
}

finished() {
    if grep -q -e 'UNHANDLED INTERRUPT' -e 'FATAL' "$LOG"; then return 0; fi
    if [ "$PASS" = foreign ]; then
        grep -q 'Aphelion is up' "$LOG"
        return $?
    fi
    grep -q -e '\[selftest\] smp-fs:' "$LOG" || return 1
    grep -q -e '\[selftest\] smp-blockdev:' "$LOG" || return 1
    grep -q -e '\[selftest\] clock ticks:' "$LOG" || return 1
    if [ "$CORES" -gt 1 ]; then
        grep -q -e 'stealing confirmed' -e 'no migration observed' "$LOG"
    else
        grep -q 'Aphelion is up' "$LOG"
    fi
}

boot() {
    PASS="$1"
    LOG="$LOG_DIR/$DISK_KIND-$CORES-$PASS.log"
    : > "$LOG"
    started="$(date +%s)"
    qemu-system-x86_64 -M q35 -cpu max -m 256M -smp "$CORES" \
        -cdrom "$ISO" $DISK -serial "file:$LOG" -display none -no-reboot >/dev/null 2>&1 &
    qpid=$!
    waited=0
    while kill -0 "$qpid" 2>/dev/null && [ "$waited" -lt "$LIMIT" ]; do
        if finished; then break; fi
        sleep 1
        waited=$((waited + 1))
    done
    kill "$qpid" 2>/dev/null || true
    wait "$qpid" 2>/dev/null || true
    tr -d '\r' < "$LOG" > "$LOG.clean"
    ELAPSED=$(( $(date +%s) - started ))
    if [ "$PASS" = fresh ]; then : > "$LOG_DIR/$DISK_KIND-$CORES.time"; fi
    echo "$PASS ${ELAPSED}s" >> "$LOG_DIR/$DISK_KIND-$CORES.time"
    echo "boot time ($DISK_KIND, $CORES core(s), $PASS): ${ELAPSED}s"
}

check_common() {
    grep -q 'Aphelion is up' "$LOG.clean" || fail "kernel did not finish booting"
    grep -q 'UNHANDLED INTERRUPT' "$LOG.clean" && fail "unhandled interrupt"
    grep -q 'FATAL' "$LOG.clean" && fail "fatal error"
    grep -q 'MISMATCH' "$LOG.clean" && fail "self-test mismatch"
    grep -q 'failed its checksum' "$LOG.clean" && fail "a bitmap or metadata sector failed its checksum on a clean boot"
    grep -q '\[stellar\] free-space bitmap:' "$LOG.clean" || fail "the free-space bitmap was not reported"
    grep -q '\[selftest\] hello.txt: ok' "$LOG.clean" || fail "hello.txt self-test did not pass"
    grep -q '\[selftest\] multi-request queueing: ok' "$LOG.clean" || fail "multi-request queueing self-test did not pass"
    grep -q '\[selftest\] flush: ok' "$LOG.clean" || fail "block-device flush self-test did not pass"
    grep -q '\[selftest\] bulk I/O: ok' "$LOG.clean" || fail "bulk I/O self-test did not pass"
    grep -q '\[selftest\] stellar api: ok' "$LOG.clean" || fail "filesystem API self-test did not pass"
    grep -q '\[selftest\] clock mtime: ok' "$LOG.clean" || fail "RTC-backed file mtime self-test did not pass"
    grep -q '\[selftest\] rename: ok' "$LOG.clean" || fail "rename self-test did not pass"
    grep -q '\[selftest\] extents: ok' "$LOG.clean" || fail "extent-table self-test did not pass"
    grep -q '\[selftest\] clock ticks: ok' "$LOG.clean" || fail "BSP tick counter self-test did not pass"
    grep -q 'stress: 14 stars created (create_ok=1), 0 mismatch(es)' "$LOG.clean" || fail "filesystem stress test failed"
    grep -q '\[smp-fs\] FAIL' "$LOG.clean" && fail "concurrent filesystem stress reported a failure"
    grep -q '\[selftest\] smp-fs: ok' "$LOG.clean" || fail "concurrent filesystem stress did not pass"
    grep -q '\[smp-blockdev\] FAIL' "$LOG.clean" && fail "concurrent block-device stress reported a failure"
    grep -q '\[selftest\] smp-blockdev: ok' "$LOG.clean" || fail "concurrent block-device stress did not pass"
    if [ "$CORES" -gt 1 ]; then
        grep -q 'stealing confirmed' "$LOG.clean" || fail "no cross-core work-stealing observed"
    fi
}

boot fresh
check_common
grep -q '\[stellar\] formatted:' "$LOG.clean" || fail "blank disk was not formatted"
grep -q 'snapshot() cost 1 write(s) + 0 read(s)' "$LOG.clean" || fail "snapshot is not O(1)"
grep -q '\[selftest\] COW and tree snapshots: ok' "$LOG.clean" || fail "snapshot self-test did not pass"

boot persisted
check_common
grep -q '\[stellar\] mounted:' "$LOG.clean" || fail "existing filesystem was not mounted"
grep -q '\[stellar\] formatted:' "$LOG.clean" && fail "existing filesystem was reformatted"
grep -q 'snapshot self-tests skipped' "$LOG.clean" || fail "mounted-disk path was not taken"

head -c 67108864 /dev/urandom > "$FIMG"
BEFORE="$(cksum < "$FIMG")"
DISK="$(disk_args "$FIMG")"
boot foreign
grep -q 'Aphelion is up' "$LOG.clean" || fail "kernel did not finish booting on a foreign disk"
grep -q 'UNHANDLED INTERRUPT' "$LOG.clean" && fail "unhandled interrupt on a foreign disk"
grep -q 'FATAL' "$LOG.clean" && fail "fatal error on a foreign disk"
grep -q 'refusing to format' "$LOG.clean" || fail "kernel did not refuse to format a foreign disk"
grep -q '\[stellar\] formatted:' "$LOG.clean" && fail "kernel formatted a foreign disk"
grep -q 'block-device stress skipped' "$LOG.clean" || fail "disk-tail stress ran without a mounted volume"
[ "$(cksum < "$FIMG")" = "$BEFORE" ] || fail "the foreign disk was modified"

echo "ok: $DISK_KIND, $CORES core(s), fresh, persisted and foreign-disk boot"
