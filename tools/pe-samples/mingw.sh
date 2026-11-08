#!/bin/sh
set -e
cd "$(dirname "$0")"
CC="${MINGW_CC:-x86_64-w64-mingw32-gcc}"
OUT="${1:-../../build/pe-mingw}"
PEINFO="${PEINFO:-../../build/peinfo}"
mkdir -p "$OUT"

cat > "$OUT/stdio.c" <<'C'
#include <stdio.h>
int main(void) {
    printf("Hello World\n");
    return 0;
}
C

cat > "$OUT/k32.c" <<'C'
typedef void* HANDLE;
typedef unsigned long DWORD;
typedef int BOOL;
__declspec(dllimport) HANDLE __stdcall GetStdHandle(DWORD);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE, const void*, DWORD, DWORD*, void*);
__declspec(dllimport) void __stdcall ExitProcess(unsigned int);

void entry(void) {
    DWORD written = 0;
    HANDLE out = GetStdHandle((DWORD)-11);
    WriteFile(out, "Hello World\n", 12, &written, 0);
    ExitProcess(written == 12 ? 0 : 1);
}
C

$CC -O2 "$OUT/stdio.c" -o "$OUT/stdio.exe"
$CC -O2 -Wl,--no-dynamicbase -Wl,--disable-reloc-section "$OUT/stdio.c" -o "$OUT/stdio-fixed.exe"
$CC -O2 -s "$OUT/stdio.c" -o "$OUT/stdio-stripped.exe"
$CC -O2 -ffreestanding -fno-stack-protector -nostdlib -Wl,-e,entry -Wl,--subsystem,console "$OUT/k32.c" -o "$OUT/k32.exe" -lkernel32

if [ -x "$PEINFO" ]; then
    for f in stdio stdio-fixed stdio-stripped k32; do
        echo "================ $f.exe"
        "$PEINFO" "$OUT/$f.exe" || true
    done
fi
