#!/bin/sh
set -eu

ISO="${ISO:-build/aphelion-shell.iso}"
CORES="${1:-2}"
LIMIT="${SHELL_TIMEOUT:-120}"
LOG_DIR="${LOG_DIR:-build/ci-logs}"
DIR="$(mktemp -d)"
QPID=""
trap 'kill "$QPID" 2>/dev/null || true; rm -rf "$DIR"' EXIT
mkdir -p "$LOG_DIR"

cat > "$DIR/session.py" <<'PY'
import os, re, socket, sys, time

scenario, directory, qpid, limit, log_path = sys.argv[1], sys.argv[2], int(sys.argv[3]), float(sys.argv[4]), sys.argv[5]
deadline = time.time() + limit
transcript = bytearray()
PROMPT = "▸"


def alive():
    try:
        os.kill(qpid, 0)
        with open("/proc/%d/stat" % qpid) as f:
            return f.read().split()[2] != "Z"
    except (OSError, IOError):
        return False


def fail(reason):
    with open(log_path, "wb") as f:
        f.write(transcript)
    tail = transcript.decode("utf-8", "replace").replace("\r", "")[-2200:]
    print("FAIL (shell %s, %s): %s\n%s" % (scenario, log_path, reason, tail))
    sys.exit(1)


def connect(name):
    path = os.path.join(directory, name)
    while time.time() < deadline:
        if os.path.exists(path):
            s = socket.socket(socket.AF_UNIX)
            try:
                s.connect(path)
                s.setblocking(False)
                return s
            except OSError:
                s.close()
        time.sleep(0.2)
    fail("QEMU never opened the %s socket" % name)


def pump(sock, seconds):
    end = time.time() + seconds
    while time.time() < end:
        try:
            data = sock.recv(4096)
            if not data:
                return False
            transcript.extend(data)
        except BlockingIOError:
            time.sleep(0.05)
    return True


def expect(sock, pattern, what, wait=20, after=None):
    rx = re.compile(pattern.encode("utf-8"), re.S)
    start = len(transcript) if after is None else after
    end = min(time.time() + wait, deadline)
    while time.time() < end:
        m = rx.search(bytes(transcript[start:]))
        if m:
            return start + m.end()
        if not pump(sock, 0.2):
            m = rx.search(bytes(transcript[start:]))
            if m:
                return start + m.end()
            break
    fail("expected %s" % what)


def send(sock, data):
    sock.sendall(data if isinstance(data, bytes) else data.encode("utf-8"))


def run(sock, line, pattern, what, wait=20):
    mark = len(transcript)
    send(sock, line + "\r")
    got = expect(sock, "\r\n" + pattern, what, wait=wait, after=mark)
    expect(sock, PROMPT, "the prompt after %s" % what, wait=wait, after=got)


def finish(serial, monitor, closing):
    send(serial, "reboot\r")
    end = time.time() + 30
    while time.time() < end and alive():
        pump(serial, 0.3)
    if alive():
        fail("the machine did not reset after the reboot command")
    text = transcript.decode("utf-8", "replace")
    for bad in ("UNHANDLED INTERRUPT", "FATAL", "MISMATCH", "KERNEL PANIC"):
        if bad in text:
            fail("the serial log contains " + bad)
    with open(log_path, "wb") as f:
        f.write(transcript)
    print("ok: " + closing)


serial = connect("serial")
monitor = connect("monitor")

expect(serial, PROMPT, "the shell prompt", wait=max(1, deadline - time.time()))
expect(serial, "storage", "the info panel", wait=5)
if scenario == "nodisk":
    run(serial, "echo café ₹ Ω > ram:/u8", "", "writing Unicode to a file")
    run(serial, "cat ram:/u8", "café ₹ Ω\r\n", "reading Unicode back")
    run(serial, "echo x > ram:/naïve-₹", "", "a Unicode file name")
    run(serial, "ls ram:/", ".*naïve-₹", "the Unicode file name in a listing")
    run(serial, "cat ram:/u8 | grep -c Ω", "1\r\n", "a pipeline")
    run(serial, "cat ram:/u8 | grep -i CAFÉ", "café ₹ Ω\r\n", "a Unicode-aware match through a pipe")
    run(serial, "realms", ".*ram.*sys", "the realm list")
    run(serial, "cat sys:/version", "Aphelion x86_64", "the sys realm version file")
    run(serial, "cat sys:/memory", ".*[0-9]", "the sys realm memory file")
    run(serial, "cat sys:/realms", ".*disk", "the sys realm list of realms")
    run(serial, "mkdir -p ram:/d/e", "", "mkdir -p")
    run(serial, "cp ram:/u8 ram:/d/e/copy", "", "cp")
    run(serial, "cat ram:/d/e/copy", "café ₹ Ω\r\n", "the copied file")
    run(serial, "cp ram:/u8 ram:/d/e/copy", ".*refus|.*exists", "cp refusing to overwrite")
    run(serial, "mv ram:/d/e/copy ram:/d/moved", "", "mv")
    run(serial, "ls ram:/d/e | grep -c .", "0\r\n", "the emptied directory")
    run(serial, "rm -r ram:/d", "", "rm -r")
    run(serial, "ls ram:/d; echo rc=$?", ".*rc=1", "ls failing on a removed directory")
    run(serial, "set v=hello; echo $v $?", "hello 0\r\n", "variables")
    run(serial, "echo a; false; echo $?", "a\r\n.*1\r\n", "exit status")
    run(serial, "echo one two three | count -w", "3\r\n", "the count command")
    run(serial, "mkfs erase", ".*no block device", "mkfs without a disk")
    run(serial, "snapshot", ".*no disk volume", "snapshot without a disk volume")
    run(serial, "cat ram:/missing", ".*not found", "an error for a missing file")
    run(serial, "ps", ".*helm", "the Satellite list")
    run(serial, "cores", ".*Sun 0", "the core list")

    send(serial, "cat ram:/u\t")
    expect(serial, "cat ram:/u8", "tab completion of a path", wait=10)
    mark = len(transcript)
    send(serial, "\r")
    expect(serial, "\r\ncafé ₹ Ω\r\n", "the completed command running", wait=10, after=mark)
    expect(serial, PROMPT, "the prompt after completion", wait=10)

    send(serial, "\x1b[A")
    expect(serial, "cat ram:/u8", "history recall with the up arrow", wait=10)
    send(serial, "\x03")
    expect(serial, "\\^C", "control-C cancelling the line", wait=10)
    expect(serial, PROMPT, "a fresh prompt after control-C", wait=10)

    run(serial, "help", ".*poweroff", "the help text")
    run(serial, "help syntax", ".*\\|", "the syntax help")

    run(serial, "echo a &&", ".*", "a malformed line being rejected without hanging", wait=10)
    mark = len(transcript)
    send(serial, "sleep 20000\r")
    time.sleep(1.5)
    send(serial, "\x03")
    expect(serial, "\\^C|interrupt", "control-C interrupting a running command", wait=10, after=mark)
    expect(serial, PROMPT, "the prompt after an interrupted command", wait=10, after=mark)

    for key in "uptime":
        send(monitor, "sendkey %s\n" % key)
        time.sleep(0.15)
    send(monitor, "sendkey ret\n")
    expect(serial, "\r\n  up [0-9]+h [0-9]+m [0-9]+s", "uptime typed on the emulated PS/2 keyboard", wait=15)
    expect(serial, PROMPT, "the prompt after the PS/2 command", wait=10)

    send(monitor, "sendkey shift-h\n")
    expect(serial, "H", "a shifted letter from the PS/2 keyboard", wait=10)
    send(serial, "\x15")

    run(serial, "klog 600", ".*\\[", "the kernel log tail")
    finish(serial, monitor, "shell mode without a disk: Unicode files, pipes, realms, completion, interrupt, PS/2 and serial input, klog and reboot")

if scenario == "disk-first":
    expect(serial, "no Stellar volume", "the info panel to report an unformatted disk", wait=5)
    run(serial, "ls disk:/", ".*not available", "disk: being unavailable before mkfs")
    run(serial, "mkfs", ".*usage: mkfs erase", "mkfs asking for the erase word")
    run(serial, "mkfs erase", ".*new Stellar volume created", "mkfs erase on a blank disk")
    run(serial, "mkfs erase", ".*refusing to erase", "mkfs refusing to erase a mounted volume")
    run(serial, "mkdir -p disk:/proj/src", "", "mkdir on the disk")
    run(serial, "echo persisted ₹ > disk:/proj/src/main", "", "writing a file to the disk")
    run(serial, "cat disk:/proj/src/main", "persisted ₹\r\n", "reading it back")
    run(serial, "snapshot", ".*snapshot 1 taken", "taking a snapshot")
    run(serial, "echo changed > disk:/proj/src/main", "", "overwriting after the snapshot")
    run(serial, "cat snap:/1/proj/src/main", "persisted ₹\r\n", "the snapshot keeping the old contents")
    run(serial, "cat disk:/proj/src/main", "changed\r\n", "the live file")
    run(serial, "rm snap:/1/proj/src/main", ".*read-only", "snapshots being read-only")
    run(serial, "cp -r snap:/1/proj disk:/restored", "", "restoring a tree from a snapshot")
    run(serial, "cat disk:/restored/src/main", "persisted ₹\r\n", "the restored file")
    run(serial, "echo big > ram:/b; cp ram:/b disk:/b; cat disk:/b", "big\r\n", "copying across realms")
    run(serial, "check -d", ".*volume is consistent", "a deep check")
    run(serial, "cat sys:/storage", ".*", "the storage description")
    run(serial, "sync", "", "sync")
    finish(serial, monitor, "shell mode on a blank disk: mkfs erase, disk and snapshot realms, deep check")

if scenario == "disk-second":
    expect(serial, "Stellar volume mounted", "the info panel to report the mounted volume", wait=5)
    run(serial, "cat disk:/proj/src/main", "changed\r\n", "the file written in the previous boot")
    run(serial, "cat disk:/restored/src/main", "persisted ₹\r\n", "the restored tree written in the previous boot")
    run(serial, "cat disk:/b", "big\r\n", "the cross-realm copy written in the previous boot")
    run(serial, "cat snap:/1/proj/src/main", "persisted ₹\r\n", "the snapshot surviving the reboot")
    run(serial, "mkfs erase", ".*refusing to erase", "mkfs refusing to erase a mounted volume after the reboot")
    run(serial, "check", ".*volume is consistent", "a check after the reboot")
    finish(serial, monitor, "shell mode on the persisted disk: files, tree restore and snapshot survived a reboot")
PY

session() {
    scenario="$1"
    disk="$2"
    rm -f "$DIR/serial" "$DIR/monitor"
    if [ -n "$disk" ]; then
        set -- -drive "file=$disk,if=none,id=d0,format=raw" -device "virtio-blk-pci,drive=d0"
    else
        set --
    fi
    qemu-system-x86_64 -M q35 -cpu max -m 256M -smp "$CORES" -cdrom "$ISO" "$@" -display none -no-reboot \
        -serial "unix:$DIR/serial,server=on,wait=off" -monitor "unix:$DIR/monitor,server=on,wait=off" >/dev/null 2>&1 &
    QPID=$!
    python3 -I "$DIR/session.py" "$scenario" "$DIR" "$QPID" "$LIMIT" "$LOG_DIR/shell-$scenario-$CORES.log"
    kill "$QPID" 2>/dev/null || true
    wait "$QPID" 2>/dev/null || true
}

session nodisk ""

qemu-img create -f raw "$DIR/disk.img" 64M >/dev/null
session disk-first "$DIR/disk.img"
session disk-second "$DIR/disk.img"
