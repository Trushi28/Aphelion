#!/bin/sh
set -e
cd "$(dirname "$0")"
W=$(mktemp -d)
CC="clang -target x86_64-pc-windows-msvc -O1 -fno-stack-protector -fno-ident -c"
LD="lld-link /nodefaultlib /timestamp:0 /brepro"
llvm-dlltool -m i386:x86-64 -d kernel32-run.def -l $W/kernel32.lib
$CC teb.c -o $W/teb.obj
$CC tlscb.c -o $W/tlscb.obj
$LD /subsystem:console /entry:mainCRTStartup /out:$W/teb.exe $W/teb.obj $W/kernel32.lib
$LD /subsystem:console /entry:mainCRTStartup /out:$W/tlscb.exe $W/tlscb.obj $W/kernel32.lib
mkdir -p bin
cp $W/teb.exe bin/teb.exe
cp $W/tlscb.exe bin/tlscb.exe
python3 - "$W" > ../pe_samples_run.hpp <<'PY'
import sys, base64
w = sys.argv[1]
print("#pragma once")
print()
items = ("teb", "tlscb")
for i, name in enumerate(items):
    text = base64.b64encode(open(w + "/" + name + ".exe", "rb").read()).decode()
    print("static const char pe_sample_%s[] =" % name)
    for k in range(0, len(text), 96):
        print('    "%s"' % text[k:k + 96])
    print("    ;")
    if i + 1 < len(items):
        print()
PY
rm -rf "$W"
