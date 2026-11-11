#!/bin/sh
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${1:-}"
if [ -z "$DEST" ]; then
    echo "usage: tools/real-binaries.sh <empty or new directory outside the repository>" >&2
    exit 2
fi
mkdir -p "$DEST"
DEST="$(cd "$DEST" && pwd)"
case "$DEST/" in
    "$ROOT"/*) echo "refusing to use a directory inside the repository: third-party binaries are never committed" >&2; exit 2 ;;
esac
for tool in build/peinfo build/pehost_host_test; do
    [ -x "$ROOT/$tool" ] || { echo "$tool is missing: run make test-pehost first" >&2; exit 2; }
done

echo "This downloads Windows wheels from PyPI and runs ninja.exe natively inside the pehost test harness."
echo "Only run it where executing those programs is acceptable. Nothing is run outside the harness."

mkdir -p "$DEST/wheels" "$DEST/x"
python3 -m pip download --quiet --no-deps --only-binary=:all: --platform win_amd64 --python-version 3.12 -d "$DEST/wheels" ninja msvc-runtime
python3 -I - "$DEST" <<'PY'
import hashlib, os, sys, zipfile
dest = sys.argv[1]
for name in sorted(os.listdir(dest + "/wheels")):
    path = dest + "/wheels/" + name
    print(hashlib.sha256(open(path, "rb").read()).hexdigest(), name)
    out = os.path.realpath(dest + "/x/" + name[:-4])
    os.makedirs(out, exist_ok=True)
    with zipfile.ZipFile(path) as z:
        for member in z.namelist():
            target = os.path.realpath(os.path.join(out, member))
            if not target.startswith(out + os.sep):
                sys.exit("unsafe path in " + name + ": " + member)
        z.extractall(out)
PY

PEINFO="$ROOT/build/peinfo"
HOST="$ROOT/build/pehost_host_test"
SUPPORTED="$ROOT/docs/pe-supported.txt"
NINJA="$(find "$DEST/x" -name ninja.exe | head -n 1)"
[ -n "$NINJA" ] || { echo "ninja.exe was not found in the wheel" >&2; exit 1; }
NDIR="$(dirname "$NINJA")"

echo
echo "== import coverage of the real programs and libraries"
"$PEINFO" --coverage --supported "$SUPPORTED" "$NINJA" || true
find "$DEST/x" -not -path '*/Scripts/*' \( -name 'vcruntime140*.dll' -o -name 'msvcp140*.dll' -o -name 'concrt140.dll' -o -name 'vcomp140.dll' \) | LC_ALL=C sort | while read -r dll; do
    "$PEINFO" --coverage --supported "$SUPPORTED" "$dll" | sed -n '1,2p' || true
done

WORK="$DEST/work"
rm -rf "$WORK"
mkdir -p "$WORK"
cp "$NINJA" "$WORK/ninja.exe"
printf 'rule say\n  command = echo hello\nbuild out.txt: say in.txt\nbuild all: phony out.txt\ndefault all\n' > "$WORK/build.ninja"
printf 'input\n' > "$WORK/in.txt"

failures=0
expect() {
    description="$1"
    shift
    if "$@"; then echo "ok    $description"; else echo "FAIL  $description"; failures=$((failures + 1)); fi
}
run() { "$HOST" --run "$WORK/ninja.exe" "ninja.exe $1" --dlls "$WORK" 2>&1; }

echo
echo "== running the real ninja.exe"
expect "ninja --version prints a version and exits 0" sh -c "'$HOST' --run '$WORK/ninja.exe' 'ninja.exe --version' --dlls '$WORK' 2>&1 | grep -q 'exit status 0'"
expect "ninja -t targets all lists the targets of build.ninja" sh -c "'$HOST' --run '$WORK/ninja.exe' 'ninja.exe -t targets all' --dlls '$WORK' 2>&1 | grep -q '^out.txt: say'"
expect "ninja -t rules lists the rules" sh -c "'$HOST' --run '$WORK/ninja.exe' 'ninja.exe -t rules' --dlls '$WORK' 2>&1 | grep -q '^say'"
expect "ninja -n plans the build by reading and statting files" sh -c "'$HOST' --run '$WORK/ninja.exe' 'ninja.exe -n' --dlls '$WORK' 2>&1 | grep -q '\[1/1\] echo hello'"
expect "ninja -t commands prints the command line" sh -c "'$HOST' --run '$WORK/ninja.exe' 'ninja.exe -t commands' --dlls '$WORK' 2>&1 | grep -q 'echo hello'"
echo
echo "known limit: a real build needs I/O completion ports, pipes and CreateProcess, which fail with ERROR_NOT_SUPPORTED:"
run "" | sed -n '1,4p' || true
echo
[ "$failures" -eq 0 ] && echo "all real-binary checks passed" || { echo "$failures real-binary check(s) failed"; exit 1; }
