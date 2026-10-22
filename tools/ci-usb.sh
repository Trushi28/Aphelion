#!/bin/sh
set -eu

ISO="${ISO:-build/aphelion-usb.iso}"
SIZE_MIB="${USB_SIZE_MIB:-96}"
LIMIT="${USB_TIMEOUT:-150}"
LOG_DIR="${LOG_DIR:-build/ci-logs}"
DIR="$(mktemp -d)"
trap 'rm -rf "$DIR"' EXIT
mkdir -p "$LOG_DIR"

cp "$ISO" "$DIR/stick.img"
truncate -s "${SIZE_MIB}M" "$DIR/stick.img"
head -c $((32 * 1024 * 1024)) /dev/urandom > "$DIR/decoy.img"

python3 -I - "$DIR" "$ISO" "$LIMIT" "$LOG_DIR/usb.log" <<'PY'
import hashlib, os, re, socket, subprocess, sys, time

directory, iso, limit, log_path = sys.argv[1], sys.argv[2], float(sys.argv[3]), sys.argv[4]
deadline = time.time() + limit
transcript = bytearray()
stick = os.path.join(directory, "stick.img")
decoy = os.path.join(directory, "decoy.img")
iso_size = os.path.getsize(iso)


def digest(path, count=None):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        h.update(f.read(count) if count else f.read())
    return h.hexdigest()


decoy_before = digest(decoy)
iso_before = digest(iso)


def fail(reason):
    with open(log_path, "wb") as f:
        f.write(transcript)
    tail = transcript.decode("utf-8", "replace").replace("\r", "")[-2200:]
    print("FAIL (usb): %s\n%s" % (reason, tail))
    sys.exit(1)


def launch(tag):
    sock_path = os.path.join(directory, "serial-" + tag)
    proc = subprocess.Popen([
        "qemu-system-x86_64", "-M", "q35", "-cpu", "max", "-m", "256M", "-smp", "2", "-display", "none", "-no-reboot",
        "-serial", "unix:%s,server=on,wait=off" % sock_path,
        "-device", "qemu-xhci,id=xhci",
        "-drive", "if=none,id=stick,file=%s,format=raw" % stick,
        "-device", "usb-storage,bus=xhci.0,drive=stick,bootindex=0",
        "-drive", "if=none,id=decoy,file=%s,format=raw" % decoy,
        "-device", "usb-storage,bus=xhci.0,drive=decoy",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    while time.time() < deadline and not os.path.exists(sock_path):
        time.sleep(0.2)
    s = socket.socket(socket.AF_UNIX)
    while time.time() < deadline:
        try:
            s.connect(sock_path)
            s.setblocking(False)
            return proc, s
        except OSError:
            time.sleep(0.2)
    proc.kill()
    fail("QEMU never opened the serial socket")


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


def expect(sock, pattern, what, wait=60, since=None):
    rx = re.compile(pattern.encode("utf-8"))
    start = len(transcript) if since is None else since
    end = min(time.time() + wait, deadline)
    while time.time() < end:
        m = rx.search(bytes(transcript[start:]))
        if m:
            return m
        if not pump(sock, 0.2):
            m = rx.search(bytes(transcript[start:]))
            if m:
                return m
            break
    fail("expected %s" % what)


def send(sock, text):
    sock.sendall(text.encode("utf-8"))


proc, serial = launch("one")
mark = len(transcript)
expect(serial, "xHCI|xhci", "the xHCI controller line", wait=90, since=mark)
expect(serial, "storage window: [0-9]+ sector", "a storage window on the booted stick", wait=90, since=mark)
expect(serial, "▸", "the shell prompt", wait=60, since=mark)

mark = len(transcript)
send(serial, "usb\r")
expect(serial, "boot stick", "the booted stick listed as such", since=mark)
expect(serial, "other disk \\(untouched\\)", "the decoy stick listed as untouched", since=mark)

send(serial, "storage\r")
expect(serial, "blank, not formatted yet", "a blank window before formatting")

send(serial, "storage init erase\r")
m = expect(serial, "storage init erase ([0-9]+)", "the confirmation instruction")
mib = m.group(1).decode()

send(serial, "storage init erase %d\r" % (int(mib) + 1))
expect(serial, "to confirm type", "a wrong size not being accepted")

send(serial, "storage init erase %s\r" % mib)
expect(serial, "window formatted and mounted", "the window being formatted after confirmation", wait=90)

send(serial, "storage init erase %s\r" % mib)
expect(serial, "already holds a Stellar volume", "a second erase being refused")

send(serial, "reboot\r")
end = time.time() + 40
while time.time() < end and proc.poll() is None:
    pump(serial, 0.3)
if proc.poll() is None:
    proc.kill()
    fail("the machine did not reset after the reboot command")

proc, serial = launch("two")
mark = len(transcript)
expect(serial, "USB window: Stellar volume mounted", "the formatted window mounting on the next boot", wait=120, since=mark)
expect(serial, "▸", "the shell prompt after the second boot", wait=60, since=mark)
send(serial, "storage\r")
expect(serial, "Stellar, mounted", "the storage command reporting the mounted volume")
send(serial, "poweroff\r")
time.sleep(2)
proc.kill()
proc.wait()

text = transcript.decode("utf-8", "replace")
for bad in ("UNHANDLED INTERRUPT", "FATAL", "MISMATCH", "KERNEL PANIC"):
    if bad in text:
        fail("the serial log contains " + bad)

if digest(decoy) != decoy_before:
    fail("the decoy stick was modified")
with open(stick, "rb") as f:
    head = f.read(iso_size)
with open(iso, "rb") as f:
    original = f.read()
if head != original:
    fail("the boot image area of the stick was modified")
if digest(iso) != iso_before:
    fail("the ISO itself changed during the run")

with open(log_path, "wb") as f:
    f.write(transcript)
print("ok: usb storage window found on the booted stick, decoy untouched, image intact, formatted on confirmation and mounted again after reboot")
PY
