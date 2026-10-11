#!/bin/sh
# Boots the kernel under QEMU and kills it with SIGKILL at random instants, then checks the crashed disk image with
# the host-side fsck and, every few rounds, boots it to completion. This exercises the real kernel, drivers and block
# layer, but a killed emulator only produces crashes that are an in-order prefix of the guest's writes: nothing is
# reordered, torn or lost from a device cache. See docs/STELLAR.md ("Crash testing").
#
# usage: tools/crash-fuzz.sh [nvme|ahci|virtio] [cores] [rounds] [seed]
set -eu

DISK_KIND="${1:-nvme}"
CORES="${2:-1}"
ROUNDS="${3:-20}"
SEED="${4:-1}"
ISO="${ISO:-build/aphelion.iso}"
STELLARFS="${STELLARFS:-build/stellarfs}"
MIN_MS="${MIN_MS:-150}"
MAX_MS="${MAX_MS:-4500}"
FRESH_EVERY="${FRESH_EVERY:-4}"
FULL_EVERY="${FULL_EVERY:-6}"
LIMIT="${BOOT_TIMEOUT:-150}"
LOG_DIR="${LOG_DIR:-build/ci-logs}"

mkdir -p "$LOG_DIR"
IMG="$(mktemp)"
CHECK_IMG="$(mktemp)"
trap 'rm -f "$IMG" "$CHECK_IMG"' EXIT

disk_args() {
    case "$DISK_KIND" in
        nvme)   echo "-drive file=$IMG,if=none,id=d0,format=raw$1 -device nvme,drive=d0,serial=aphelion0" ;;
        ahci)   echo "-drive file=$IMG,if=none,id=d0,format=raw$1 -device ide-hd,drive=d0,bus=ide.0" ;;
        virtio) echo "-drive file=$IMG,if=none,id=d0,format=raw$1 -device virtio-blk-pci,drive=d0" ;;
        *) echo "unknown disk kind: $DISK_KIND" >&2; exit 2 ;;
    esac
}
DISK="$(disk_args "")"
THROTTLED="$(disk_args ",throttling.iops-total=${FORMAT_IOPS:-200}")"

MAGIC="c5 a2 11 61 7a 15 e1 5a"
magic_at() { od -An -v -tx1 -j "$1" -N8 "$IMG" | tr -d '\n' | sed 's/^ *//'; }

new_image() { rm -f "$IMG"; qemu-img create -f raw "$IMG" 64M >/dev/null; }

start_qemu() {
    qemu-system-x86_64 -M q35 -cpu max -m 256M -smp "$CORES" -cdrom "$ISO" ${DRIVE:-$DISK} \
        -serial "file:$1" -display none -no-reboot >/dev/null 2>&1 &
    qpid=$!
}

stop_qemu() {
    kill -9 "$qpid" 2>/dev/null || true
    wait "$qpid" 2>/dev/null || true
}

keep_failure() {
    cp --sparse=always "$IMG" "$LOG_DIR/crash-fuzz-failed-$DISK_KIND-$CORES-round$1.img"
    echo "FAIL (round $1, $DISK_KIND, $CORES core(s)): $2"
    echo "  crashed image kept at $LOG_DIR/crash-fuzz-failed-$DISK_KIND-$CORES-round$1.img, serial log $3"
    exit 1
}

clean_count=0; unformatted=0; leaked_total=0; full_boots=0; kills_before_fs=0

SHORT="$(awk -v seed="$((SEED + 1000))" -v n="$ROUNDS" -v hi="${FORMAT_WINDOW_MS:-90}" 'BEGIN { srand(seed); for (i = 0; i < n; i++) printf "%.3f\n", (rand() * hi) / 1000 }')"
DELAYS="$(awk -v seed="$SEED" -v n="$ROUNDS" -v lo="$MIN_MS" -v hi="$MAX_MS" 'BEGIN { srand(seed); for (i = 0; i < n; i++) printf "%.3f\n", (lo + rand() * (hi - lo)) / 1000 }')"

round=0
marker_kills=0
for delay in $DELAYS; do
    round=$((round + 1))
    fresh=0
    if [ "$round" -eq 1 ] || [ $((round % FRESH_EVERY)) -eq 0 ]; then new_image; fresh=1; fi
    LOG="$LOG_DIR/crash-fuzz-$DISK_KIND-$CORES.log"
    : > "$LOG"
    if [ "$fresh" -eq 1 ]; then
        DRIVE="$THROTTLED" start_qemu "$LOG"
        short="$(echo "$SHORT" | sed -n "${round}p")"
        waited=0
        while ! grep -q 'blank disk: formatting' "$LOG" && kill -0 "$qpid" 2>/dev/null && [ "$waited" -lt 3000 ]; do
            sleep 0.005
            waited=$((waited + 1))
        done
        sleep "$short"
        marker_kills=$((marker_kills + 1))
    else
        start_qemu "$LOG"
        sleep "$delay"
    fi
    stop_qemu

    cp --sparse=always "$IMG" "$CHECK_IMG"
    out="$("$STELLARFS" fsck "$CHECK_IMG" 2>&1 || true)"
    if echo "$out" | grep -q '^fsck: clean'; then
        clean_count=$((clean_count + 1))
        leaked="$(echo "$out" | sed -n 's/.*leaked sectors \([0-9][0-9]*\).*/\1/p' | head -1)"
        leaked_total=$((leaked_total + ${leaked:-0}))
    elif echo "$out" | grep -q 'mount failed: not formatted'; then
        unformatted=$((unformatted + 1))
        if ! cmp -s -n 32768 "$IMG" /dev/zero && [ "$(magic_at 0)" != "$MAGIC" ] && [ "$(magic_at 512)" != "$MAGIC" ]; then
            keep_failure "$round" "the disk is neither blank nor marked as ours, so the kernel would refuse to format it" "$LOG"
        fi
    else
        echo "$out" | tail -5
        keep_failure "$round" "fsck of the crashed image failed" "$LOG"
    fi

    if [ $((round % FULL_EVERY)) -eq 0 ] || [ "$round" -eq "$ROUNDS" ]; then
        FLOG="$LOG_DIR/crash-fuzz-full-$DISK_KIND-$CORES.log"
        : > "$FLOG"
        start_qemu "$FLOG"
        waited=0
        while kill -0 "$qpid" 2>/dev/null && [ "$waited" -lt "$LIMIT" ]; do
            if grep -q -e 'UNHANDLED INTERRUPT' -e 'FATAL' "$FLOG"; then break; fi
            if grep -q '\[selftest\] smp-fs:' "$FLOG" && grep -q '\[selftest\] smp-blockdev:' "$FLOG" && grep -q 'Aphelion is up' "$FLOG"; then break; fi
            sleep 1
            waited=$((waited + 1))
        done
        stop_qemu
        tr -d '\r' < "$FLOG" > "$FLOG.clean"
        for want in 'Aphelion is up' '\[selftest\] hello.txt: ok' '\[selftest\] rename: ok' '\[selftest\] extents: ok' '\[selftest\] clock mtime: ok' '\[selftest\] smp-fs: ok' '\[selftest\] smp-blockdev: ok'; do
            grep -q "$want" "$FLOG.clean" || { tail -20 "$FLOG.clean"; keep_failure "$round" "the crashed disk did not boot to a passing self-test run: missing '$want'" "$FLOG.clean"; }
        done
        for bad in 'UNHANDLED INTERRUPT' 'FATAL' 'MISMATCH' 'refusing to format' 'failed its checksum' '\[smp-fs\] FAIL' '\[smp-blockdev\] FAIL'; do
            if grep -q "$bad" "$FLOG.clean"; then grep -m3 "$bad" "$FLOG.clean"; keep_failure "$round" "the crashed disk booted with '$bad'" "$FLOG.clean"; fi
        done
        full_boots=$((full_boots + 1))
        cp --sparse=always "$IMG" "$CHECK_IMG"
        "$STELLARFS" fsck "$CHECK_IMG" 2>&1 | grep -q '^fsck: clean' || keep_failure "$round" "fsck after a completed recovery boot is not clean" "$FLOG.clean"
    fi
done

echo "ok: $DISK_KIND, $CORES core(s), $ROUNDS kills (seed $SEED, $marker_kills aimed at the format window): $clean_count fsck-clean, $unformatted left unformatted, $leaked_total leaked sectors left for gc across all crashes, $full_boots full recovery boots passed"
