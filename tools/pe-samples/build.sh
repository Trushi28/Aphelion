#!/bin/sh
set -e
cd "$(dirname "$0")"
W=$(mktemp -d)
CC="clang -target x86_64-pc-windows-msvc -O1 -fno-stack-protector -fno-ident -c"
LD="lld-link /nodefaultlib /timestamp:0 /brepro"
llvm-dlltool -m i386:x86-64 -d kernel32.def -l $W/kernel32.lib
llvm-dlltool -m i386:x86-64 -d mylib.def -l $W/mylib.lib
$CC console.c -o $W/console.obj
$CC ordinals.c -o $W/ordinals.obj
$CC library.c -o $W/library.obj
$LD /subsystem:console /entry:mainCRTStartup /out:$W/console.exe $W/console.obj $W/kernel32.lib
$LD /subsystem:console /entry:mainCRTStartup /dynamicbase:no /fixed /out:$W/fixed.exe $W/console.obj $W/kernel32.lib
$LD /subsystem:console /entry:mainCRTStartup /out:$W/ordinals.exe $W/ordinals.obj $W/mylib.lib $W/kernel32.lib
$LD /dll /noentry /out:$W/library.dll $W/library.obj
python3 - "$W" > ../pe_samples.hpp <<'PY'
import sys, base64
w = sys.argv[1]
print("#pragma once")
print()
for name, ext in (("console", "exe"), ("fixed", "exe"), ("ordinals", "exe"), ("library", "dll")):
    text = base64.b64encode(open(w + "/" + name + "." + ext, "rb").read()).decode()
    print("static const char pe_sample_%s[] =" % name)
    for i in range(0, len(text), 96):
        print('    "%s"' % text[i:i + 96])
    print("    ;")
    if name != "library":
        print()
PY
rm -rf "$W"
