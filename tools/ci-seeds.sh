#!/bin/sh
set -u

if [ "$#" -lt 3 ]; then
    echo "usage: tools/ci-seeds.sh <nvme|ahci|virtio> <cores> <seed> [seed ...]" >&2
    exit 2
fi

DISK_KIND="$1"
CORES="$2"
shift 2
FAILED=""

for SEED in "$@"; do
    case "$SEED" in
        ''|*[!0-9]*|0)
            echo "seed must be a positive decimal number: $SEED" >&2
            exit 2
            ;;
    esac
    ISO="build/aphelion-seed-$SEED.iso"
    LOGS="build/ci-logs/seed-$SEED"
    mkdir -p "$LOGS"
    if ! make iso BOOT_DEFAULT=test TEST_SEED="$SEED" ISO="$ISO" MENU_TIMEOUT=0 >"$LOGS/build.log" 2>&1; then
        echo "FAIL (seed $SEED): the ISO did not build, see $LOGS/build.log"
        FAILED="$FAILED $SEED"
        continue
    fi
    if ! ISO="$ISO" LOG_DIR="$LOGS" tools/ci-boot.sh "$DISK_KIND" "$CORES"; then
        echo "FAIL (seed $SEED): boot or self-test did not pass, logs in $LOGS"
        FAILED="$FAILED $SEED"
        continue
    fi
    if ! grep -aq "syscall fuzzer.*status 0" "$LOGS/$DISK_KIND-$CORES-fresh.log"; then
        echo "FAIL (seed $SEED): the fuzzer line is missing or not status 0 in $LOGS"
        FAILED="$FAILED $SEED"
        continue
    fi
    if ! grep -aq "syscalls succeeded at least once, seed $SEED" "$LOGS/$DISK_KIND-$CORES-fresh.log"; then
        echo "FAIL (seed $SEED): the run did not report the seed it was given, see $LOGS"
        FAILED="$FAILED $SEED"
        continue
    fi
    echo "ok: seed $SEED, $DISK_KIND, $CORES core(s)"
done

if [ -n "$FAILED" ]; then
    echo "failed seeds:$FAILED"
    exit 1
fi
