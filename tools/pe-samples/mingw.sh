#!/bin/sh
set -e
cd "$(dirname "$0")"
CC="${MINGW_CC:-x86_64-w64-mingw32-gcc} -Wl,--no-insert-timestamp"
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

cat > "$OUT/crt.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <windows.h>

extern IMAGE_DOS_HEADER __ImageBase;

static int failures;

static void check(int ok, const char *what) {
    if (!ok) {
        failures++;
        printf("FAIL %s\n", what);
    }
}

static void first_handler(void) { printf("exit handler 1\n"); }
static void second_handler(void) { printf("exit handler 2\n"); }

int main(int argc, char **argv) {
    char text[96];
    char small[8];
    wchar_t wide[16];
    char narrow[16];
    int i, n;

    printf("argc=%d\n", argc);
    for (i = 0; i < argc; i++) printf("argv[%d]=<%s>\n", i, argv[i]);

    atexit(first_handler);
    atexit(second_handler);

    {
        char *a = malloc(100);
        char *z;
        memset(a, 'x', 100);
        a = realloc(a, 5000);
        check(a != NULL && a[0] == 'x' && a[99] == 'x', "realloc keeps the old bytes");
        z = calloc(10, 8);
        for (i = 0, n = 0; i < 80; i++) n |= z[i];
        check(z != NULL && n == 0, "calloc returns zeroed memory");
        free(a);
        free(z);
        check(malloc(64u << 20) == NULL, "an allocation past the heap cap fails");
    }

    n = sprintf(text, "%5d|%-5d|%05d|%x|%X|%#x|%s|%c|%%|%ld|%I64d", 42, 42, 42, 255, 255, 255, "str", 'c', -7L, 123456789012LL);
    check(strcmp(text, "   42|42   |00042|ff|FF|0xff|str|c|%|-7|123456789012") == 0, "sprintf formats integers, strings and characters");
    check(n == (int)strlen(text), "sprintf returns the length");
    n = _snprintf(small, sizeof small, "%s", "0123456789");
    check(n == -1, "_snprintf reports truncation");
    n = _snprintf(small, sizeof small, "%s", "01234567");
    check(n == 8, "_snprintf with an exact fit returns the size");
    printf("direct %d %s %5.2s|\n", 5, "msvcrt", "abcdef");
    fprintf(stderr, "to stderr %u\n", 3u);

    check(strlen("hello") == 5 && strcmp("a", "b") < 0 && strncmp("abcd", "abxx", 2) == 0, "string compare and length");
    check(strchr("hello", 'l') != NULL && strrchr("hello", 'l') - "hello" == 3 && strstr("hello", "llo") != NULL, "string search");
    strcpy(text, "ab");
    strcat(text, "cd");
    check(strcmp(text, "abcd") == 0 && memcmp("abc", "abd", 3) < 0, "string copy and concatenation");

    check(signal(SIGINT, SIG_IGN) == SIG_DFL && signal(SIGINT, SIG_DFL) == SIG_IGN, "signal returns the previous handler");
    errno = 0;
    check(strlen(strerror(EINVAL)) > 0 && getenv("NO_SUCH_VARIABLE") == NULL, "strerror and getenv");

    {
        HANDLE heap = GetProcessHeap();
        void *p = HeapAlloc(heap, HEAP_ZERO_MEMORY, 64);
        check(p != NULL && HeapSize(heap, 0, p) >= 64, "HeapAlloc and HeapSize");
        p = HeapReAlloc(heap, 0, p, 4096);
        check(p != NULL && HeapSize(heap, 0, p) >= 4096, "HeapReAlloc grows");
        check(HeapFree(heap, 0, p) != 0, "HeapFree");
    }

    {
        DWORD slot = TlsAlloc();
        check(slot != TLS_OUT_OF_INDEXES, "TlsAlloc");
        check(TlsSetValue(slot, (void *)0x1234) && TlsGetValue(slot) == (void *)0x1234, "TlsSetValue and TlsGetValue");
        check(TlsFree(slot) != 0 && TlsFree(slot) == 0, "TlsFree once");
    }

    {
        CRITICAL_SECTION cs;
        InitializeCriticalSection(&cs);
        EnterCriticalSection(&cs);
        EnterCriticalSection(&cs);
        check(cs.RecursionCount == 2, "a critical section counts recursion");
        LeaveCriticalSection(&cs);
        LeaveCriticalSection(&cs);
        check(cs.RecursionCount == 0, "and releases it");
        DeleteCriticalSection(&cs);
    }

    {
        HMODULE self = GetModuleHandleA(NULL);
        HMODULE k32 = GetModuleHandleA("KERNEL32");
        check(self != NULL && (char *)self == (char *)&__ImageBase, "GetModuleHandle(NULL) is the image base");
        check(k32 != NULL && GetProcAddress(k32, "GetLastError") != NULL && GetProcAddress(k32, "NoSuchFunction") == NULL, "GetProcAddress");
        check(GetModuleHandleA("nosuch.dll") == NULL, "an unknown module is not found");
        check(strstr(GetCommandLineA(), "pe.exe") != NULL, "GetCommandLineA");
        check(GetModuleFileNameA(NULL, text, sizeof text) > 0, "GetModuleFileNameA");
    }

    n = MultiByteToWideChar(CP_UTF8, 0, "h\xc3\xa9llo \xf0\x9f\x98\x80", -1, wide, 16);
    check(n == 9 && wide[1] == 0x00E9 && wide[6] == 0xD83D && wide[7] == 0xDE00, "MultiByteToWideChar decodes UTF-8 including a surrogate pair");
    n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, sizeof narrow, NULL, NULL);
    check(n == 12 && (unsigned char)narrow[1] == 0xC3, "WideCharToMultiByte encodes it back");
    check(WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, 4, NULL, NULL) == 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "a short output buffer is reported");

    printf("done failures=%d\n", failures);
    return failures ? 100 + failures : 7;
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
$CC -O1 -s -D__USE_MINGW_ANSI_STDIO=0 "$OUT/crt.c" -o "$OUT/crt.exe"
$CC -O2 -Wl,--no-dynamicbase -Wl,--disable-reloc-section "$OUT/stdio.c" -o "$OUT/stdio-fixed.exe"
$CC -O2 -s "$OUT/stdio.c" -o "$OUT/stdio-stripped.exe"
$CC -O2 -ffreestanding -fno-stack-protector -nostdlib -Wl,-e,entry -Wl,--subsystem,console "$OUT/k32.c" -o "$OUT/k32.exe" -lkernel32

if [ -x "$PEINFO" ]; then
    for f in stdio stdio-fixed stdio-stripped crt k32; do
        echo "================ $f.exe"
        "$PEINFO" "$OUT/$f.exe" || true
    done
fi
