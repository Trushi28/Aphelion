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
    got = expect(sock, "\r\n" + pattern.replace(".*", ".*?"), what, wait=wait, after=mark)
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
if scenario == "nodisk":
    run(serial, "echo café ₹ Ω > ram:/u8", "", "writing Unicode to a file")
    run(serial, "cat ram:/u8", "café ₹ Ω\r\n", "reading Unicode back")
    run(serial, "echo x > ram:/naïve-₹", "", "a Unicode file name")
    run(serial, "ls ram:/", ".*naïve-₹", "the Unicode file name in a listing")
    run(serial, "cat ram:/u8 | grep -c Ω", "1\r\n", "a pipeline")
    run(serial, "cat ram:/u8 | grep -i CAF", ".*caf.*é ₹ Ω\r\n", "a case-folded match through a pipe")
    run(serial, "cat ram:/u8 | grep -c ₹", "1\r\n", "a non-ASCII match through a pipe")
    run(serial, "realms", ".*sys.*ram.*disk.*snap", "the realm list")
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
    run(serial, "echo one two three | count -w", " *3\r\n", "the count command")
    run(serial, "mkfs erase", ".*no block device", "mkfs without a disk")
    run(serial, "snapshot", ".*no disk volume", "snapshot without a disk volume")
    run(serial, "cat ram:/missing", ".*not found", "an error for a missing file")
    run(serial, "mkdir -p ram:/lt/a/b; cd ram:/lt/a/b; pwd", "ram:/lt/a/b\r\n", "pwd after cd into a nested directory")
    run(serial, "cd ../..; pwd", "ram:/lt\r\n", "cd .. walking up the parent lenses")
    run(serial, "echo hi > a/f; cat a/f", "hi\r\n", "a relative path resolved from the current lens")
    run(serial, "cd a/b; cat ../f; cd -; pwd", "hi\r\n.*ram:/lt\r\n", "a relative read through .. and cd -")
    run(serial, "cd ../../../..; pwd", "ram:/\r\n", "cd above the root stopping at the root")
    run(serial, "cd ram:/lt; touch ./c:d; ls", ".*c:d", "a file name with a colon")
    run(serial, "stat sys:/version", ".*realm +sys, read-only", "a probe reporting its read-only realm")
    run(serial, "echo x > sys:/version", ".*read-only", "a write to a probe being refused")
    run(serial, "cat sys:/uptime", ".*uptime_ms: [0-9]+", "the uptime probe as key: value text")
    run(serial, "cat sys:/cpus", ".*sun\\.0: apic=[0-9]+ state=", "the cpus probe, one line per sun")
    run(serial, "cat sys:/tasks", ".*task\\.[0-9]+: state=.*name=helm", "the tasks probe, one line per satellite")
    run(serial, "rm -r ram:/lt; cd ram:/", "", "cleaning up")
    run(serial, "ps", ".*helm", "the Satellite list")
    run(serial, "cores", ".*Sun 0", "the core list")
    run(serial, "ps | where name == helm | cat", ".*task\\.[0-9]+: state=[a-z]+ ring=[0-9]+ core=[0-9]+ switches=[0-9]+ name=helm", "ps piped as parseable rows, filtered by a field")
    run(serial, "ps | sort -r -k switches | head -n 1 | cat", ".*task\\.[0-9]+: state=", "ps rows sorted by a numeric field")
    run(serial, "ps | pick name state | head -n 1 | cat", ".*task\\.[0-9]+: name=.* state=", "pick keeping only the named fields")
    run(serial, "ps | where state == running", ".*id +state +ring +core +switches +name", "a terminal getting an aligned table")
    run(serial, "tty", ".*console: kind=stream rights=rw[-a-z]* readable=(yes|no) writable=yes", "the console lens the shell holds")
    run(serial, "cat sys:/log | head -n 1 | cat", ".*log\\.[0-9]+: ", "the kernel log as a keyed probe")

    send(serial, "cat ram:/u\t")
    expect(serial, "cat ram:/u8", "tab completion of a path", wait=10)
    mark = len(transcript)
    send(serial, "\r")
    got = expect(serial, "\r\ncafé ₹ Ω\r\n", "the completed command running", wait=10, after=mark)
    expect(serial, PROMPT, "the prompt after completion", wait=10, after=got)

    send(serial, "\x1b[A")
    expect(serial, "cat ram:/u8", "history recall with the up arrow", wait=10)
    send(serial, "\x03")
    got = expect(serial, "\\^C", "control-C cancelling the line", wait=10)
    expect(serial, PROMPT, "a fresh prompt after control-C", wait=10, after=got)

    run(serial, "help", ".*poweroff", "the help text")
    run(serial, "help syntax", ".*\\|", "the syntax help")

    run(serial, "echo a &&", ".*", "a malformed line being rejected without hanging", wait=10)
    mark = len(transcript)
    send(serial, "sleep 20000\r")
    time.sleep(1.5)
    send(serial, "\x03")
    got = expect(serial, "\\^C|interrupt", "control-C interrupting a running command", wait=10, after=mark)
    expect(serial, PROMPT, "the prompt after an interrupted command", wait=10, after=got)

    for key in "uptime":
        send(monitor, "sendkey %s\n" % key)
        time.sleep(0.15)
    send(monitor, "sendkey ret\n")
    got = expect(serial, "\r\n  up [0-9]+h [0-9]+m [0-9]+s", "uptime typed on the emulated PS/2 keyboard", wait=15)
    expect(serial, PROMPT, "the prompt after the PS/2 command", wait=10, after=got)

    send(monitor, "sendkey shift-h\n")
    expect(serial, "H", "a shifted letter from the PS/2 keyboard", wait=10)
    send(serial, "\x15")

    run(serial, "klog 600", ".*\\[", "the kernel log tail")
    finish(serial, monitor, "shell mode without a disk: Unicode files, pipes, realms, completion, interrupt, PS/2 and serial input, klog and reboot")

if scenario == "disk-first":
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
    run(serial, "cd snap:/1/proj/src; cat main; pwd", "persisted ₹\r\n.*snap:/1/proj/src\r\n", "a relative read inside a snapshot")
    run(serial, "cd disk:/proj/src; cat main; cd ..; pwd", "changed\r\n.*disk:/proj\r\n", "relative paths on the disk")
    run(serial, "cd ram:/", "", "back to ram")
    run(serial, "rm snap:/1/proj/src/main", ".*read-only", "snapshots being read-only")
    run(serial, "cp -r snap:/1/proj disk:/restored", "", "restoring a tree from a snapshot")
    run(serial, "cat disk:/restored/src/main", "persisted ₹\r\n", "the restored file")
    run(serial, "echo big > ram:/b; cp ram:/b disk:/b; cat disk:/b", "big\r\n", "copying across realms")
    run(serial, "check -d", ".*volume is consistent", "a deep check")
    run(serial, "cat sys:/storage", ".*", "the storage description")
    run(serial, "sync", "", "sync")
    finish(serial, monitor, "shell mode on a blank disk: mkfs erase, disk and snapshot realms, deep check")

if scenario == "disk-second":
    run(serial, "cat disk:/proj/src/main", "changed\r\n", "the file written in the previous boot")
    run(serial, "cat disk:/restored/src/main", "persisted ₹\r\n", "the restored tree written in the previous boot")
    run(serial, "cat disk:/b", "big\r\n", "the cross-realm copy written in the previous boot")
    run(serial, "cd disk:/proj/src; cat main; pwd", "changed\r\n.*disk:/proj/src\r\n", "the previous boot's file by a relative path")
    run(serial, "cd ram:/", "", "back to ram")
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
