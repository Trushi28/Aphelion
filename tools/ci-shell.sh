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
    send(serial, "restart\r")
    end = time.time() + 30
    while time.time() < end and alive():
        pump(serial, 0.3)
    if alive():
        fail("the machine did not reset after the restart command")
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
    run(serial, "say café ₹ Ω | etch ram:/u8", "", "writing Unicode to a file")
    run(serial, "beacon ram:/u8", "café ₹ Ω\r\n", "reading Unicode back")
    run(serial, "say x | etch ram:/naïve-₹", "", "a Unicode file name")
    run(serial, "chart ram:/", ".*naïve-₹", "the Unicode file name in a chart")
    run(serial, "beacon ram:/u8 | seek Ω | tally", ".*rows.*\r\n *1\r\n", "text flowing into rows and being counted")
    run(serial, "beacon ram:/u8 | seek fold: yes CAF | pick text", ".*café ₹ Ω\r\n", "a case-folded match through a pipe")
    run(serial, "beacon ram:/u8 | seek ₹ | pick line", ".*line.*\r\n *1\r\n", "a non-ASCII match reporting its line")
    run(serial, "realms", ".*sys.*ram.*disk.*snap", "the realm rows")
    run(serial, "realms | where realm = sys | pick access", "read-only\r\n", "realm rows filtered by a typed column")
    run(serial, "beacon sys:/version", ".*Aphelion x86_64", "the sys realm version file")
    run(serial, "beacon sys:/memory", ".*[0-9]", "the sys realm memory file")
    run(serial, "beacon sys:/realms", ".*disk", "the sys realm list of realms")
    run(serial, "plant parents: yes ram:/d/e", "", "plant with parents")
    run(serial, "twin ram:/u8 ram:/d/e/copy", "", "twin")
    run(serial, "beacon ram:/d/e/copy", "café ₹ Ω\r\n", "the copied file")
    run(serial, "twin ram:/u8 ram:/d/e/copy", ".*already exists", "twin refusing to overwrite")
    run(serial, "drift ram:/d/e/copy ram:/d/moved", "", "drift")
    run(serial, "chart ram:/d/e", ".*0 rows", "the emptied directory")
    run(serial, "fade tree: yes ram:/d", "", "fade tree")
    run(serial, "chart ram:/d !> say rc=@status", ".*rc=1", "chart failing on a removed directory, then the else link")
    run(serial, "bind v hello; say @v @status", "hello 0\r\n", "bound values")
    run(serial, "say a; fail; say @status", "a\r\n.*1\r\n", "status")
    run(serial, "ok => say then !> say else", "then\r\n", "the then link")
    run(serial, "fail => say then !> say else", ".*else\r\n", "the else link")
    run(serial, "say one two three | tally", ".*lines.*words.*\r\n *1 +3 +14 +14\r\n", "counting text")
    run(serial, "kindle erase", ".*no block device", "kindle without a disk")
    run(serial, "epoch take", ".*no disk volume", "epoch without a disk volume")
    run(serial, "beacon ram:/missing", ".*not found", "an error for a missing file")
    run(serial, "cat ram:/u8", ".*unknown command: cat", "Unix command names being absent")
    run(serial, "chart depth: 0", ".*depth: takes a number", "a named option out of range")
    run(serial, "chart dpth: 1", ".*does not take the option dpth:", "an unknown named option")
    run(serial, "plant parents: yes ram:/lt/a/b; go ram:/lt/a/b; here", "ram:/lt/a/b\r\n", "here after go into a nested directory")
    run(serial, "go ../..; here", "ram:/lt\r\n", "go .. walking up the parent lenses")
    run(serial, "say hi | etch a/f; beacon a/f", "hi\r\n", "a relative path resolved from the current lens")
    run(serial, "go a/b; beacon ../f; back; here", "hi\r\n.*ram:/lt\r\n", "a relative read through .. and back")
    run(serial, "go ../../../..; here", "ram:/\r\n", "go above the root stopping at the root")
    run(serial, "go ram:/lt; spark ./c:d; chart", ".*c:d", "a file name with a colon")
    run(serial, "probe sys:/version", ".*sys.*read-only", "a probe reporting its read-only realm")
    run(serial, "say x | etch sys:/version", ".*read-only", "a write to a probe being refused")
    run(serial, "beacon sys:/uptime", ".*uptime_ms: [0-9]+", "the uptime probe as key: value text")
    run(serial, "beacon sys:/cpus", ".*sun\\.0: apic=[0-9]+ state=", "the cpus probe, one line per sun")
    run(serial, "beacon sys:/tasks", ".*task\\.[0-9]+: state=.*name=helm", "the tasks probe, one line per satellite")
    run(serial, "fade tree: yes ram:/lt; go ram:/", "", "cleaning up")
    run(serial, "orbit | where name = helm | pick name", ".*helm\r\n", "orbit rows filtered by name")
    run(serial, "orbit | where ring >= 0", ".*id +state +ring +sun +switches +name", "a terminal getting an aligned table")
    run(serial, "orbit | order switches down: yes | first 1 | pick id", ".*id.*\r\n *[0-9]+\r\n", "orbit rows ordered by a number")
    run(serial, "orbit | where switches > 0 | pick name state | first 1", ".*name +state", "pick keeping only the named columns")
    run(serial, "suns", ".*sun +apic +state +queued +running", "the sun rows")
    run(serial, "memory", ".*universe.*heap.*ram realm", "the memory rows")
    run(serial, "console", ".*console: kind=stream rights=rw[-a-z]* readable=(yes|no) writable=yes", "the console lens the shell holds")
    run(serial, "beacon sys:/log | first 1", ".*log\\.[0-9]+: ", "the kernel log as a keyed probe")

    send(serial, "beacon ram:/u\t")
    expect(serial, "beacon ram:/u8", "tab completion of a path", wait=10)
    mark = len(transcript)
    send(serial, "\r")
    got = expect(serial, "\r\ncafé ₹ Ω\r\n", "the completed command running", wait=10, after=mark)
    expect(serial, PROMPT, "the prompt after completion", wait=10, after=got)

    send(serial, "\x1b[A")
    expect(serial, "beacon ram:/u8", "history recall with the up arrow", wait=10)
    send(serial, "\x03")
    got = expect(serial, "\\^C", "control-C cancelling the line", wait=10)
    expect(serial, PROMPT, "a fresh prompt after control-C", wait=10, after=got)

    run(serial, "recall | last 2", ".*command.*beacon ram:/u8.*recall", "history as rows")
    run(serial, "help", ".*sunset", "the help text")
    run(serial, "help syntax", ".*=>", "the syntax help")

    run(serial, "say a =>", ".*missing command", "a malformed line being rejected without hanging", wait=10)
    mark = len(transcript)
    send(serial, "wait 20000\r")
    time.sleep(1.5)
    send(serial, "\x03")
    got = expect(serial, "\\^C|interrupt", "control-C interrupting a running command", wait=10, after=mark)
    expect(serial, PROMPT, "the prompt after an interrupted command", wait=10, after=got)

    for key in "age":
        send(monitor, "sendkey %s\n" % key)
        time.sleep(0.15)
    send(monitor, "sendkey ret\n")
    got = expect(serial, "\r\n  up [0-9]+h [0-9]+m [0-9]+s", "age typed on the emulated PS/2 keyboard", wait=15)
    expect(serial, PROMPT, "the prompt after the PS/2 command", wait=10, after=got)

    send(monitor, "sendkey shift-h\n")
    expect(serial, "H", "a shifted letter from the PS/2 keyboard", wait=10)
    send(serial, "\x15")

    run(serial, "log 600", ".*\\[", "the kernel log tail")
    finish(serial, monitor, "shell mode without a disk: Unicode files, typed rows, links, realms, completion, interrupt, PS/2 and serial input, log and restart")

if scenario == "disk-first":
    run(serial, "chart disk:/", ".*0 rows", "a blank disk being formatted and mounted at boot")
    run(serial, "kindle", ".*usage: kindle erase", "kindle asking for the erase word")
    run(serial, "kindle erase", ".*refusing to erase", "kindle refusing to erase the volume created at boot")
    run(serial, "plant parents: yes disk:/proj/src", "", "plant on the disk")
    run(serial, "say persisted ₹ | etch disk:/proj/src/main", "", "writing a file to the disk")
    run(serial, "beacon disk:/proj/src/main", "persisted ₹\r\n", "reading it back")
    run(serial, "epoch take", ".*epoch 1 taken", "taking an epoch")
    run(serial, "epoch", ".*epoch.*path.*\r\n.*1 +snap:/1", "the epoch rows")
    run(serial, "say changed | etch disk:/proj/src/main", "", "overwriting after the epoch")
    run(serial, "beacon snap:/1/proj/src/main", "persisted ₹\r\n", "the epoch keeping the old contents")
    run(serial, "beacon disk:/proj/src/main", "changed\r\n", "the live file")
    run(serial, "go snap:/1/proj/src; beacon main; here", "persisted ₹\r\n.*snap:/1/proj/src\r\n", "a relative read inside an epoch")
    run(serial, "go disk:/proj/src; beacon main; go ..; here", "changed\r\n.*disk:/proj\r\n", "relative paths on the disk")
    run(serial, "go ram:/", "", "back to ram")
    run(serial, "fade snap:/1/proj/src/main", ".*read-only", "epochs being read-only")
    run(serial, "twin tree: yes snap:/1/proj disk:/restored", "", "restoring a tree from an epoch")
    run(serial, "beacon disk:/restored/src/main", "persisted ₹\r\n", "the restored file")
    run(serial, "say big | etch ram:/b; twin ram:/b disk:/b; beacon disk:/b", "big\r\n", "copying across realms")
    run(serial, "chart disk:/ | where kind = dir | pick name", ".*proj.*restored", "directories on the disk found with a typed filter")
    run(serial, "audit deep: yes", ".*volume is consistent", "a deep audit")
    run(serial, "beacon sys:/storage", ".*", "the storage description")
    run(serial, "settle", "", "settle")
    finish(serial, monitor, "shell mode on a blank disk formatted at boot: disk and epoch realms, deep audit")

if scenario == "disk-second":
    run(serial, "beacon disk:/proj/src/main", "changed\r\n", "the file written in the previous boot")
    run(serial, "beacon disk:/restored/src/main", "persisted ₹\r\n", "the restored tree written in the previous boot")
    run(serial, "beacon disk:/b", "big\r\n", "the cross-realm copy written in the previous boot")
    run(serial, "go disk:/proj/src; beacon main; here", "changed\r\n.*disk:/proj/src\r\n", "the previous boot's file by a relative path")
    run(serial, "go ram:/", "", "back to ram")
    run(serial, "beacon snap:/1/proj/src/main", "persisted ₹\r\n", "the epoch surviving the restart")
    run(serial, "kindle erase", ".*refusing to erase", "kindle refusing to erase a mounted volume after the restart")
    run(serial, "audit", ".*volume is consistent", "an audit after the restart")
    finish(serial, monitor, "shell mode on the persisted disk: files, tree restore and epoch survived a restart")
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
