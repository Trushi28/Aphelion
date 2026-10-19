#!/bin/sh
set -eu

ISO="${ISO:-build/aphelion-shell.iso}"
CORES="${1:-2}"
LIMIT="${SHELL_TIMEOUT:-120}"
LOG_DIR="${LOG_DIR:-build/ci-logs}"
DIR="$(mktemp -d)"
trap 'kill "$QPID" 2>/dev/null || true; rm -rf "$DIR"' EXIT
mkdir -p "$LOG_DIR"

qemu-system-x86_64 -M q35 -cpu max -m 256M -smp "$CORES" -cdrom "$ISO" -display none -no-reboot \
    -serial "unix:$DIR/serial,server=on,wait=off" -monitor "unix:$DIR/monitor,server=on,wait=off" >/dev/null 2>&1 &
QPID=$!

python3 -I - "$DIR" "$QPID" "$LIMIT" "$LOG_DIR/shell-$CORES.log" <<'PY'
import os, re, socket, sys, time

directory, qpid, limit, log_path = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
deadline = time.time() + limit
transcript = bytearray()


def alive():
    try:
        os.kill(qpid, 0)
        with open("/proc/%d/stat" % qpid) as f:
            return f.read().split()[2] != "Z"
    except (OSError, IOError):
        return False


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


def fail(reason):
    with open(log_path, "wb") as f:
        f.write(transcript)
    tail = transcript.decode("utf-8", "replace").replace("\r", "")[-1800:]
    print("FAIL (shell, %s): %s\n%s" % (sys.argv[4], reason, tail))
    sys.exit(1)


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


def expect(sock, pattern, what, wait=20):
    rx = re.compile(pattern.encode("utf-8"))
    start = len(transcript)
    end = min(time.time() + wait, deadline)
    while time.time() < end:
        if rx.search(bytes(transcript[start:])):
            return
        if not pump(sock, 0.2) and not rx.search(bytes(transcript[start:])):
            break
    fail("expected %s" % what)


def send(sock, data):
    sock.sendall(data if isinstance(data, bytes) else data.encode("utf-8"))


serial = connect("serial")
monitor = connect("monitor")

expect(serial, "aphelion ▸", "the shell prompt", wait=max(1, deadline - time.time()))
expect(serial, "storage", "the info panel", wait=5)

send(serial, "echo café ₹ Ω\r")
expect(serial, "\r\ncafé ₹ Ω\r\n", "the echo command output over the serial keyboard path")

send(serial, "ps\r")
expect(serial, "helm-probe", "the Satellite list")

send(serial, "cores\r")
expect(serial, "Sun 0", "the core list")

send(serial, "\x1b[A")
expect(serial, "cores", "history recall with the up arrow", wait=10)
send(serial, "\x03")
expect(serial, "\\^C", "control-C cancelling the line")

send(serial, "help\r")
expect(serial, "poweroff", "the help text")

for key in "uptime":
    send(monitor, "sendkey %s\n" % key)
    time.sleep(0.15)
send(monitor, "sendkey ret\n")
expect(serial, "\r\n  up [0-9]+h [0-9]+m [0-9]+s", "uptime typed on the emulated PS/2 keyboard", wait=15)

send(monitor, "sendkey shift-h\n")
expect(serial, "H", "a shifted letter from the PS/2 keyboard", wait=10)
send(serial, "\x15")

send(serial, "klog 600\r")
expect(serial, "\\[", "the kernel log tail")

send(serial, "reboot\r")
end = time.time() + 30
while time.time() < end and alive():
    pump(serial, 0.3) if serial else None
if alive():
    fail("the machine did not reset after the reboot command")

text = transcript.decode("utf-8", "replace")
for bad in ("UNHANDLED INTERRUPT", "FATAL", "MISMATCH", "KERNEL PANIC"):
    if bad in text:
        fail("the serial log contains " + bad)
with open(log_path, "wb") as f:
    f.write(transcript)
print("ok: shell mode, serial and PS/2 input, history, klog and reboot")
PY
