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

cat > "$OUT/ucrt.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <windows.h>

static int failures;

static void check(int ok, const char *what) {
    if (!ok) {
        failures++;
        printf("FAIL %s\n", what);
    }
}

static void first_handler(void) { printf("exit handler 1\n"); }
static void second_handler(void) { printf("exit handler 2\n"); }

typedef size_t (__cdecl *strlen_fn)(const char *);

int main(int argc, char **argv) {
    char text[96];
    char small[8];
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

    n = sprintf(text, "%5d|%-5d|%05d|%x|%X|%#x|%s|%c|%%|%ld|%lld", 42, 42, 42, 255, 255, 255, "str", 'c', -7L, 123456789012LL);
    check(strcmp(text, "   42|42   |00042|ff|FF|0xff|str|c|%|-7|123456789012") == 0, "sprintf formats integers, strings and characters");
    check(n == (int)strlen(text), "sprintf returns the length");

    n = snprintf(small, sizeof small, "%s", "0123456789");
    check(n == 10 && strcmp(small, "0123456") == 0, "snprintf returns the full length and always terminates");
    n = snprintf(small, sizeof small, "%s", "01234567");
    check(n == 8 && strcmp(small, "0123456") == 0, "snprintf with an exact fit loses the last character to the terminator");
    n = snprintf(NULL, 0, "%d-%d", 12, 345);
    check(n == 6, "snprintf with no buffer measures");
    n = _snprintf(small, sizeof small, "%s", "0123456789");
    check(n == -1, "_snprintf reports truncation");

    printf("direct %d %s %5.2s|\n", 5, "ucrt", "abcdef");
    fprintf(stderr, "to stderr %u\n", 3u);
    puts("a line");
    fputs("no newline;", stdout);
    putchar('\n');
    fflush(stdout);

    check(strlen("hello") == 5 && strcmp("a", "b") < 0 && strncmp("abcd", "abxx", 2) == 0, "string compare and length");
    check(strchr("hello", 'l') != NULL && strrchr("hello", 'l') - "hello" == 3 && strstr("hello", "llo") != NULL, "string search");
    strcpy(text, "ab");
    strcat(text, "cd");
    check(strcmp(text, "abcd") == 0 && memcmp("abc", "abd", 3) < 0, "string copy and concatenation");

    check(signal(SIGINT, SIG_IGN) == SIG_DFL && signal(SIGINT, SIG_DFL) == SIG_IGN, "signal returns the previous handler");
    errno = 0;
    check(strlen(strerror(EINVAL)) > 0 && getenv("NO_SUCH_VARIABLE") == NULL, "strerror and getenv");

    {
        HMODULE ucrt = GetModuleHandleA("ucrtbase.dll");
        HMODULE viaset = GetModuleHandleA("api-ms-win-crt-string-l1-1-0.dll");
        HMODULE loaded = LoadLibraryA("C:\\Windows\\System32\\ucrtbase.dll");
        strlen_fn fn;
        check(ucrt != NULL && ucrt == viaset && ucrt == loaded, "ucrtbase is one module under its real name, an api set name and a path");
        fn = (strlen_fn)GetProcAddress(ucrt, "strlen");
        check(fn != NULL && fn("four") == 4, "GetProcAddress on ucrtbase returns a callable function");
        check(GetProcAddress(ucrt, "NoSuchFunction") == NULL, "an unknown export is not found");
        check(LoadLibraryA("nosuchlib.dll") == NULL && GetLastError() == ERROR_MOD_NOT_FOUND, "LoadLibrary of an unknown library fails with ERROR_MOD_NOT_FOUND");
        check(FreeLibrary(loaded) != 0, "FreeLibrary");
    }

    printf("done failures=%d\n", failures);
    return failures ? 100 + failures : 7;
}
C

cat > "$OUT/dllmath.c" <<'C'
#include <windows.h>
#include <stdio.h>

static int calls;

__declspec(dllexport) int add(int a, int b) {
    calls++;
    return a + b;
}

__declspec(dllexport) int call_count(void) { return calls; }

__declspec(dllexport) int exported_value = 41;

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    (void)h;
    printf("dllmath reason=%lu reserved=%d\n", (unsigned long)reason, reserved != NULL);
    return TRUE;
}
C

cat > "$OUT/dllchain.c" <<'C'
#include <windows.h>
#include <stdio.h>

__declspec(dllimport) int add(int a, int b);

int add3(int a, int b, int c) { return add(add(a, b), c); }

int plus_one(int a) { return a + 1; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    (void)h;
    printf("dllchain reason=%lu reserved=%d\n", (unsigned long)reason, reserved != NULL);
    return TRUE;
}
C

cat > "$OUT/dllplug.c" <<'C'
#include <windows.h>
#include <stdio.h>

__declspec(dllexport) int plug_value(void) { return 99; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    (void)h;
    printf("dllplug reason=%lu reserved=%d\n", (unsigned long)reason, reserved != NULL);
    return TRUE;
}
C

cat > "$OUT/dllfail.c" <<'C'
#include <windows.h>

__declspec(dllexport) int never(void) { return 0; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    (void)h;
    (void)reserved;
    return reason == DLL_PROCESS_ATTACH ? FALSE : TRUE;
}
C

cat > "$OUT/dyn.c" <<'C'
#include <windows.h>
#include <stdio.h>

__declspec(dllimport) int add3(int a, int b, int c);

typedef int(__cdecl *add_fn)(int, int);
typedef int(__cdecl *value_fn)(void);
typedef int(__cdecl *step_fn)(int);

static int failures;

static void check(int ok, const char *what) {
    if (!ok) {
        failures++;
        printf("FAIL %s\n", what);
    }
}

int main(void) {
    HMODULE chain = GetModuleHandleA("dllchain.dll");
    HMODULE math = GetModuleHandleA("DLLMATH");
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    HMODULE plug;
    add_fn add;
    step_fn step;
    value_fn plug_value;
    int *data;
    char name[64];

    printf("start\n");
    check(chain != NULL && math != NULL && chain != math, "both static dependencies are loaded");
    check(add3(1, 2, 3) == 6, "a call through two DLLs");
    add = (add_fn)GetProcAddress(math, "add");
    check(add != NULL && add(40, 2) == 42, "GetProcAddress by name");
    check(GetProcAddress(math, MAKEINTRESOURCEA(1)) == (FARPROC)add, "the same function by ordinal");
    check(GetProcAddress(chain, "fwd_add") == (FARPROC)add, "a forwarder into another DLL resolves to the real function");
    check(GetProcAddress(chain, "fwd_sleep") == GetProcAddress(k32, "Sleep"), "a forwarder into kernel32 resolves to the stub");
    step = (step_fn)GetProcAddress(chain, MAKEINTRESOURCEA(9));
    check(step != NULL && step(4) == 5, "an export that was given an explicit ordinal");
    data = (int *)GetProcAddress(math, "exported_value");
    check(data != NULL && *data == 41, "exported data");
    SetLastError(0);
    check(GetProcAddress(math, "nothere") == NULL && GetLastError() == ERROR_PROC_NOT_FOUND, "an unknown export");
    check(GetProcAddress(math, MAKEINTRESOURCEA(99)) == NULL, "an ordinal past the table");
    check(GetModuleFileNameA(math, name, sizeof name) > 0 && name[0] == 'd', "the name of a loaded module");
    check(LoadLibraryA("C:\\somewhere\\DllMath.dll") == math, "a loaded DLL is found again by any spelling of its name");
    check(FreeLibrary(math) != 0 && GetModuleHandleA("dllmath.dll") == math, "FreeLibrary balances it and the static import keeps it loaded");

    plug = LoadLibraryA("dllplug.dll");
    check(plug != NULL, "a DLL loaded at run time");
    plug_value = (value_fn)GetProcAddress(plug, "plug_value");
    check(plug_value != NULL && plug_value() == 99, "a call into it");
    check(FreeLibrary(plug) != 0 && GetModuleHandleA("dllplug.dll") == NULL, "it is unloaded with its last reference");
    plug = LoadLibraryA("dllplug.dll");
    check(plug != NULL, "and can be loaded again");

    SetLastError(0);
    check(LoadLibraryA("nosuch.dll") == NULL && GetLastError() == ERROR_MOD_NOT_FOUND, "an unknown DLL is ERROR_MOD_NOT_FOUND");
    check(LoadLibraryA("dllfail.dll") == NULL && GetLastError() == ERROR_DLL_INIT_FAILED, "a DLL whose DllMain fails is ERROR_DLL_INIT_FAILED");
    check(GetModuleHandleA("dllfail.dll") == NULL, "and is not left loaded");

    printf("done failures=%d\n", failures);
    return failures ? 100 + failures : 7;
}
C

cat > "$OUT/dllchain.def" <<'D'
LIBRARY dllchain.dll
EXPORTS
    add3
    plus_one @9
    fwd_add = dllmath.add
    fwd_sleep = KERNEL32.Sleep
D

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

cat > "$OUT/gaps.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int order(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

int main(void) {
    int v[3] = { 3, 1, 2 };
    qsort(v, 3, sizeof(int), order);
    FILE *f = fopen("x.txt", "r");
    printf("%d %ld %p\n", v[0], strtol("12", NULL, 10), (void *)f);
    return (int)time(NULL) & 1;
}
C

$CC -O2 "$OUT/stdio.c" -o "$OUT/stdio.exe"
$CC -O1 -s -D__USE_MINGW_ANSI_STDIO=0 "$OUT/gaps.c" -o "$OUT/gaps.exe"
$CC -O1 -s -D__USE_MINGW_ANSI_STDIO=0 "$OUT/crt.c" -o "$OUT/crt.exe"
"${MINGW_CC:-x86_64-w64-mingw32-gcc}" -Wl,--no-insert-timestamp -D_UCRT -O1 -s "$OUT/ucrt.c" -o "$OUT/ucrt.exe" -lucrt
$CC -O1 -s -D__USE_MINGW_ANSI_STDIO=0 -shared "$OUT/dllmath.c" -o "$OUT/dllmath.dll"
$CC -O1 -s -D__USE_MINGW_ANSI_STDIO=0 -shared "$OUT/dllchain.c" "$OUT/dllchain.def" "$OUT/dllmath.dll" -o "$OUT/dllchain.dll"
$CC -O1 -s -D__USE_MINGW_ANSI_STDIO=0 -shared "$OUT/dllplug.c" -o "$OUT/dllplug.dll"
$CC -O1 -s -shared -nostdlib -Wl,-e,DllMain "$OUT/dllfail.c" -o "$OUT/dllfail.dll"
$CC -O1 -s -D__USE_MINGW_ANSI_STDIO=0 "$OUT/dyn.c" "$OUT/dllchain.dll" -o "$OUT/dyn.exe"
$CC -O2 -Wl,--no-dynamicbase -Wl,--disable-reloc-section "$OUT/stdio.c" -o "$OUT/stdio-fixed.exe"
$CC -O2 -s "$OUT/stdio.c" -o "$OUT/stdio-stripped.exe"
$CC -O2 -ffreestanding -fno-stack-protector -nostdlib -Wl,-e,entry -Wl,--subsystem,console "$OUT/k32.c" -o "$OUT/k32.exe" -lkernel32

if [ -x "$PEINFO" ]; then
    for f in stdio stdio-fixed stdio-stripped crt ucrt k32 dyn; do
        echo "================ $f.exe"
        "$PEINFO" "$OUT/$f.exe" || true
    done
fi
