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
        check(strstr(GetCommandLineA(), ".exe") != NULL, "GetCommandLineA");
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

cat > "$OUT/sysio.c" <<'C'
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int failures;
static LARGE_INTEGER frequency;

static void check(int ok, const char *what) {
    if (!ok) {
        failures++;
        printf("FAIL %s\n", what);
    }
}

static unsigned long long micros(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    unsigned long long n = (unsigned long long)c.QuadPart;
    unsigned long long f = (unsigned long long)frequency.QuadPart;
    return n / f * 1000000ull + n % f * 1000000ull / f;
}

static unsigned long long as_u64(FILETIME t) {
    return ((unsigned long long)t.dwHighDateTime << 32) | t.dwLowDateTime;
}

int main(void) {
    check(QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0, "QueryPerformanceFrequency");
    unsigned long long t0 = micros();
    ULONGLONG k0 = GetTickCount64();
    FILETIME w0, w1;
    GetSystemTimeAsFileTime(&w0);
    Sleep(120);
    unsigned long long t1 = micros();
    ULONGLONG k1 = GetTickCount64();
    GetSystemTimeAsFileTime(&w1);
    check(t1 > t0 && t1 - t0 >= 100000 && t1 - t0 < 5000000, "the performance counter measures a sleep");
    check(k1 >= k0 + 100 && k1 - k0 < 5000, "the tick count measures a sleep");
    check(as_u64(w1) >= as_u64(w0) + 1000000 && as_u64(w1) - as_u64(w0) < 50000000, "the system time advances by about the sleep");
    printf("sysio clock: %lld Hz, slept %llu us, ticks %llu ms\n", (long long)frequency.QuadPart, t1 - t0, (unsigned long long)(k1 - k0));

    HANDLE h = CreateFileA("A:\\sysio.exe", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    check(h != INVALID_HANDLE_VALUE, "open the program file");
    LARGE_INTEGER size;
    size.QuadPart = 0;
    if (h != INVALID_HANDLE_VALUE) {
        unsigned char buf[600];
        DWORD got = 0;
        check(GetFileType(h) == FILE_TYPE_DISK && GetFileSizeEx(h, &size) && size.QuadPart > 1024, "file type and size");
        check(ReadFile(h, buf, 2, &got, NULL) && got == 2 && buf[0] == 'M' && buf[1] == 'Z', "the file starts with MZ");
        LARGE_INTEGER to;
        to.QuadPart = 0x3C;
        check(SetFilePointerEx(h, to, NULL, FILE_BEGIN) && ReadFile(h, buf, 4, &got, NULL) && got == 4, "read the PE header offset");
        to.QuadPart = (LONGLONG)(buf[0] | (buf[1] << 8) | (buf[2] << 16) | ((unsigned)buf[3] << 24));
        check(SetFilePointerEx(h, to, NULL, FILE_BEGIN) && ReadFile(h, buf, 4, &got, NULL) && got == 4 && memcmp(buf, "PE\0\0", 4) == 0, "the PE signature is where the header says");
        to.QuadPart = 0;
        SetFilePointerEx(h, to, NULL, FILE_BEGIN);
        LONGLONG total = 0;
        while (ReadFile(h, buf, sizeof(buf), &got, NULL) && got > 0) total += got;
        check(total == size.QuadPart, "reading to the end returns exactly the file size");
        to.QuadPart = -1;
        SetLastError(0);
        check(!SetFilePointerEx(h, to, NULL, FILE_BEGIN) && GetLastError() == ERROR_NEGATIVE_SEEK, "a negative seek fails");
        check(CloseHandle(h), "close");
        check(!CloseHandle(h) && GetLastError() == ERROR_INVALID_HANDLE, "closing twice fails");
    }

    WIN32_FIND_DATAA fd;
    HANDLE s = FindFirstFileA("A:\\*.exe", &fd);
    int found_self = 0;
    int listed = 0;
    DWORD self_size = 0;
    if (s != INVALID_HANDLE_VALUE) {
        do {
            listed++;
            if (strcmp(fd.cFileName, "sysio.exe") == 0) {
                found_self = 1;
                self_size = fd.nFileSizeLow;
            }
        } while (FindNextFileA(s, &fd));
        check(GetLastError() == ERROR_NO_MORE_FILES, "the end of a search is ERROR_NO_MORE_FILES");
        FindClose(s);
    }
    check(listed >= 1 && found_self && (LONGLONG)self_size == size.QuadPart, "a search lists the program with its size");
    printf("sysio search: %d programs\n", listed);

    SetLastError(0);
    h = CreateFileA("nosuch.bin", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    check(h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND, "a missing file is ERROR_FILE_NOT_FOUND");
    h = CreateFileA("A:\\sysio.exe", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    check(h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED, "writing is denied");
    check(GetFileAttributesA("A:\\sysio.exe") != INVALID_FILE_ATTRIBUTES && GetFileAttributesA("nosuch.bin") == INVALID_FILE_ATTRIBUTES, "file attributes");

    char env[16];
    check(SetEnvironmentVariableA("Aphelion", "yes") && GetEnvironmentVariableA("APHELION", env, sizeof(env)) == 3 && strcmp(env, "yes") == 0, "environment variables ignore case");
    DWORD slot = FlsAlloc(NULL);
    check(slot != FLS_OUT_OF_INDEXES && FlsSetValue(slot, (PVOID)42) && FlsGetValue(slot) == (PVOID)42 && FlsFree(slot), "fiber local storage");
    SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lock);
    check(!TryAcquireSRWLockExclusive(&lock), "an exclusive lock is exclusive");
    ReleaseSRWLockExclusive(&lock);
    check(TryAcquireSRWLockExclusive(&lock), "and can be taken again");
    ReleaseSRWLockExclusive(&lock);

    printf("sysio done failures=%d\n", failures);
    return failures ? 1 : 6;
}
C

cat > "$OUT/files.c" <<'C'
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int failures;
static char big[20000];
static char back[20000];

static void check(int ok, const char *what) {
    if (!ok) {
        failures++;
        printf("FAIL %s\n", what);
    }
}

static int is_dir(const char *path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static int is_file(const char *path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static int put(const char *path, const void *data, DWORD length) {
    DWORD done = 0;
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    int ok;
    if (h == INVALID_HANDLE_VALUE) return 0;
    ok = WriteFile(h, data, length, &done, NULL) && done == length;
    return CloseHandle(h) && ok;
}

static long get(const char *path, void *buf, DWORD cap) {
    DWORD got = 0;
    long total = 0;
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    while ((DWORD)total < cap && ReadFile(h, (char *)buf + total, cap - (DWORD)total, &got, NULL) && got > 0) total += (long)got;
    CloseHandle(h);
    return total;
}

static DWORD open_error(const char *path, DWORD access, DWORD disposition, DWORD flags) {
    HANDLE h;
    SetLastError(0);
    h = CreateFileA(path, access, 0, NULL, disposition, FILE_ATTRIBUTE_NORMAL | flags, NULL);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    CloseHandle(h);
    return 0;
}

static int same_wide(const WCHAR *a, const WCHAR *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int list(const char *pattern, int *directories) {
    WIN32_FIND_DATAA fd;
    int count = 0;
    HANDLE s = FindFirstFileA(pattern, &fd);
    if (directories) *directories = 0;
    if (s == INVALID_HANDLE_VALUE) return -1;
    do {
        count++;
        if (directories && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) (*directories)++;
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) count += 1000;
    } while (FindNextFileA(s, &fd));
    FindClose(s);
    return count;
}

int main(void) {
    char text[64];
    char *part = NULL;
    DWORD got = 0;
    DWORD n;
    HANDLE h;
    int i, count, dirs;
    BY_HANDLE_FILE_INFORMATION info;
    FILETIME ft;

    check(!is_file("ftest") && !is_dir("ftest"), "the work directory starts without the test directory");
    check(CreateDirectoryA("ftest", NULL) && is_dir("ftest"), "CreateDirectory makes a directory");
    SetLastError(0);
    check(!CreateDirectoryA("ftest", NULL) && GetLastError() == ERROR_ALREADY_EXISTS, "creating it again is ERROR_ALREADY_EXISTS");
    SetLastError(0);
    check(!CreateDirectoryA("ftest\\a\\b", NULL) && GetLastError() == ERROR_PATH_NOT_FOUND, "a directory under a missing one is ERROR_PATH_NOT_FOUND");
    check(CreateDirectoryA("ftest\\a", NULL) && CreateDirectoryA("ftest/a/b", NULL) && is_dir("ftest\\a\\b") && is_dir("ftest/a/b"), "nested directories, with either separator");
    check(open_error("ftest\\a*b", GENERIC_WRITE, CREATE_NEW, 0) == ERROR_INVALID_NAME, "a wildcard in a file name is ERROR_INVALID_NAME");

    check(put("ftest\\hello.txt", "hello, world\n", 13), "create a file and write it");
    check(get("ftest\\hello.txt", back, sizeof(back)) == 13 && memcmp(back, "hello, world\n", 13) == 0, "the file reads back as written");
    h = CreateFileA("ftest\\hello.txt", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    check(h != INVALID_HANDLE_VALUE && GetFileSize(h, NULL) == 13 && GetFileType(h) == FILE_TYPE_DISK, "size and type of an open file");
    memset(&info, 0, sizeof(info));
    check(h != INVALID_HANDLE_VALUE && GetFileInformationByHandle(h, &info) && info.nFileSizeLow == 13 && info.nFileSizeHigh == 0 &&
          (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0, "GetFileInformationByHandle describes a file");
    check(h != INVALID_HANDLE_VALUE && GetFileTime(h, NULL, NULL, &ft) && (ft.dwHighDateTime | ft.dwLowDateTime) != 0, "GetFileTime gives a time");
    SetLastError(0);
    check(h != INVALID_HANDLE_VALUE && !WriteFile(h, "x", 1, &got, NULL) && GetLastError() == ERROR_ACCESS_DENIED, "writing through a read-only handle is ERROR_ACCESS_DENIED");
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

    check(open_error("ftest\\hello.txt", GENERIC_WRITE, CREATE_NEW, 0) == ERROR_FILE_EXISTS, "CREATE_NEW on an existing file is ERROR_FILE_EXISTS");
    check(open_error("ftest\\none.txt", GENERIC_READ, OPEN_EXISTING, 0) == ERROR_FILE_NOT_FOUND, "a missing file is ERROR_FILE_NOT_FOUND");
    check(open_error("ftest\\nodir\\none.txt", GENERIC_READ, OPEN_EXISTING, 0) == ERROR_PATH_NOT_FOUND, "a missing directory is ERROR_PATH_NOT_FOUND");
    check(open_error("ftest\\hello.txt\\x", GENERIC_READ, OPEN_EXISTING, 0) == ERROR_PATH_NOT_FOUND, "a file used as a directory is ERROR_PATH_NOT_FOUND");
    check(open_error("ftest\\hello.txt\\x", GENERIC_WRITE, CREATE_NEW, 0) == ERROR_PATH_NOT_FOUND, "creating under a file is ERROR_PATH_NOT_FOUND");
    check(open_error("ftest\\none.txt", GENERIC_WRITE, TRUNCATE_EXISTING, 0) == ERROR_FILE_NOT_FOUND && !is_file("ftest\\none.txt"), "TRUNCATE_EXISTING does not create");

    SetLastError(0);
    h = CreateFileA("ftest\\new.txt", GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    check(h != INVALID_HANDLE_VALUE && GetLastError() == 0, "OPEN_ALWAYS creates a missing file without an error");
    check(h != INVALID_HANDLE_VALUE && WriteFile(h, "abc", 3, &got, NULL) && got == 3 && CloseHandle(h), "write the new file");
    SetLastError(0);
    h = CreateFileA("ftest\\new.txt", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    check(h != INVALID_HANDLE_VALUE && GetLastError() == ERROR_ALREADY_EXISTS && GetFileSize(h, NULL) == 3, "OPEN_ALWAYS on an existing file keeps it and sets ERROR_ALREADY_EXISTS");
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    SetLastError(0);
    h = CreateFileA("ftest\\new.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    check(h != INVALID_HANDLE_VALUE && GetLastError() == ERROR_ALREADY_EXISTS && GetFileSize(h, NULL) == 0, "CREATE_ALWAYS on an existing file empties it and sets ERROR_ALREADY_EXISTS");
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    check(put("ftest\\new.txt", "abc", 3), "write it again");

    h = CreateFileA("ftest\\pos.bin", GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    check(h != INVALID_HANDLE_VALUE, "create a file for reading and writing");
    if (h != INVALID_HANDLE_VALUE) {
        memset(back, 0, 16);
        check(WriteFile(h, "0123456789", 10, &got, NULL) && got == 10, "write ten bytes");
        check(SetFilePointer(h, 0, NULL, FILE_END) == 10 && WriteFile(h, "AB", 2, &got, NULL) && GetFileSize(h, NULL) == 12, "append at the end");
        check(SetFilePointer(h, 3, NULL, FILE_BEGIN) == 3 && WriteFile(h, "xyz", 3, &got, NULL) && got == 3, "overwrite in the middle");
        check(SetFilePointer(h, 0, NULL, FILE_BEGIN) == 0 && ReadFile(h, back, 12, &got, NULL) && got == 12 && memcmp(back, "012xyz6789AB", 12) == 0, "the middle write landed and nothing else changed");
        check(SetFilePointer(h, 5, NULL, FILE_BEGIN) == 5 && SetEndOfFile(h) && GetFileSize(h, NULL) == 5, "SetEndOfFile truncates at the position");
        check(SetFilePointer(h, 0, NULL, FILE_BEGIN) == 0 && ReadFile(h, back, 16, &got, NULL) && got == 5 && memcmp(back, "012xy", 5) == 0, "the truncated file keeps its start");
        check(SetFilePointer(h, -2, NULL, FILE_END) == 3, "seek back from the end");
        check(CloseHandle(h), "close");
    }

    h = CreateFileA("ftest\\seq.txt", GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    check(h != INVALID_HANDLE_VALUE && WriteFile(h, "ab", 2, &got, NULL) && WriteFile(h, "cd", 2, &got, NULL) && WriteFile(h, "ef", 2, &got, NULL) && GetFileSize(h, NULL) == 6, "writes one after another follow each other");
    if (h != INVALID_HANDLE_VALUE) {
        memset(back, 0, 16);
        check(SetFilePointer(h, 0, NULL, FILE_BEGIN) == 0 && ReadFile(h, back, 4, &got, NULL) && got == 4 && ReadFile(h, back + 4, 4, &got, NULL) && got == 2 && memcmp(back, "abcdef", 6) == 0, "reads one after another follow each other");
        check(ReadFile(h, back, 4, &got, NULL) && got == 0, "a read at the end returns nothing");
        CloseHandle(h);
    }
    check(DeleteFileA("ftest\\seq.txt"), "delete it");
    SetLastError(0);
    check(!CreateDirectoryA("\\", NULL) && GetLastError() == ERROR_ACCESS_DENIED, "the root cannot be created");

    for (i = 0; i < (int)sizeof(big); i++) big[i] = (char)(i * 7 + i / 251);
    check(put("ftest\\big.bin", big, sizeof(big)), "one write of twenty thousand bytes");
    check(get("ftest\\big.bin", back, sizeof(back)) == (long)sizeof(big) && memcmp(back, big, sizeof(big)) == 0, "and one read of them returns every byte");

    check(GetCurrentDirectoryA(sizeof(text), text) == 1 && strcmp(text, "\\") == 0, "the working directory starts at the root");
    check(SetCurrentDirectoryA("ftest") && GetCurrentDirectoryA(sizeof(text), text) == 6 && strcmp(text, "\\ftest") == 0, "SetCurrentDirectory enters a directory");
    check(is_file("hello.txt") && is_dir("a\\b") && is_file("..\\ftest\\hello.txt") && is_file("\\ftest\\hello.txt") && is_file("C:\\ftest\\hello.txt") && is_dir(".") && is_dir(".."), "names resolve against it, and absolutely");
    check(is_dir("..\\..\\..") && is_file("..\\..\\ftest\\hello.txt"), "dot-dot stops at the root");
    SetLastError(0);
    check(!SetCurrentDirectoryA("nodir") && GetLastError() == ERROR_FILE_NOT_FOUND, "entering a missing directory fails");
    SetLastError(0);
    check(!SetCurrentDirectoryA("hello.txt") && GetLastError() == ERROR_DIRECTORY, "entering a file fails with ERROR_DIRECTORY");
    count = list("*", &dirs);
    check(count == 5 && dirs == 1, "it holds four files and one directory and no dot entries");
    check(SetCurrentDirectoryA("a\\b") && GetCurrentDirectoryA(sizeof(text), text) == 10 && strcmp(text, "\\ftest\\a\\b") == 0, "enter a nested directory");
    check(GetCurrentDirectoryA(3, text) == 11, "a buffer that is too small is told the size it needs");
    check(SetCurrentDirectoryA("\\") && GetCurrentDirectoryA(sizeof(text), text) == 1, "back to the root");

    n = GetFullPathNameA("ftest\\a\\..\\hello.txt", sizeof(text), text, &part);
    check(n == 16 && strcmp(text, "\\ftest\\hello.txt") == 0 && part != NULL && strcmp(part, "hello.txt") == 0, "GetFullPathName removes dot-dot and finds the file part");

    check(list("ftest\\*", &dirs) == 5 && dirs == 1, "FindFirstFile lists a subdirectory");
    check(list("ftest\\*.bin", NULL) == 2 && list("ftest\\h*", NULL) == 1 && list("ftest\\hello.txt", NULL) == 1, "patterns select by name");
    check(list("ftest\\a\\*", &dirs) == 1 && dirs == 1 && list("\\ftest\\a\\b\\*", NULL) == -1, "a nested search and an empty directory");
    SetLastError(0);
    check(list("ftest\\zzz*", NULL) == -1 && GetLastError() == ERROR_FILE_NOT_FOUND, "no match is ERROR_FILE_NOT_FOUND");
    SetLastError(0);
    check(list("nodir\\*", NULL) == -1 && GetLastError() == ERROR_PATH_NOT_FOUND, "a missing directory is ERROR_PATH_NOT_FOUND");
    check(list("\\*", &dirs) >= 1 && dirs >= 1, "the root lists the test directory");

    check(CreateDirectoryA("ftest\\many", NULL), "a directory for many files");
    for (i = 0; i < 40; i++) {
        sprintf(text, "ftest\\many\\f%02d.txt", i);
        if (!put(text, text, (DWORD)strlen(text))) break;
    }
    check(i == 40, "forty files are created");
    {
        WIN32_FIND_DATAA fd;
        int seen[40];
        int total = 0, repeated = 0;
        HANDLE s;
        memset(seen, 0, sizeof(seen));
        s = FindFirstFileA("ftest\\many\\*", &fd);
        if (s != INVALID_HANDLE_VALUE) {
            do {
                int k = (fd.cFileName[1] - '0') * 10 + (fd.cFileName[2] - '0');
                total++;
                if (strlen(fd.cFileName) == 7 && fd.cFileName[0] == 'f' && k >= 0 && k < 40) {
                    if (seen[k]) repeated++;
                    seen[k] = 1;
                } else repeated++;
            } while (FindNextFileA(s, &fd));
            FindClose(s);
        }
        for (i = 0; i < 40; i++) total -= seen[i];
        check(total == 0 && repeated == 0, "a search returns each of the forty exactly once");
    }
    for (i = 0; i < 40; i++) {
        sprintf(text, "ftest\\many\\f%02d.txt", i);
        if (!DeleteFileA(text)) break;
    }
    check(i == 40 && RemoveDirectoryA("ftest\\many") && !is_dir("ftest\\many"), "and they are deleted one by one with their directory");

    check(MoveFileA("ftest\\hello.txt", "ftest\\a\\b\\moved.txt") && !is_file("ftest\\hello.txt") && get("ftest\\a\\b\\moved.txt", back, sizeof(back)) == 13, "MoveFile moves a file into another directory");
    SetLastError(0);
    check(!MoveFileA("ftest\\none.txt", "ftest\\x.txt") && GetLastError() == ERROR_FILE_NOT_FOUND, "moving a missing file is ERROR_FILE_NOT_FOUND");
    SetLastError(0);
    check(!MoveFileA("ftest\\new.txt", "ftest\\pos.bin") && GetLastError() == ERROR_ALREADY_EXISTS && is_file("ftest\\new.txt"), "moving onto an existing file is ERROR_ALREADY_EXISTS");
    check(MoveFileExA("ftest\\new.txt", "ftest\\pos.bin", MOVEFILE_REPLACE_EXISTING) && !is_file("ftest\\new.txt") && get("ftest\\pos.bin", back, sizeof(back)) == 3, "MOVEFILE_REPLACE_EXISTING replaces it");
    check(MoveFileA("ftest\\a", "ftest\\renamed") && !is_dir("ftest\\a") && get("ftest\\renamed\\b\\moved.txt", back, sizeof(back)) == 13, "a directory is renamed with what is in it");

    check(CopyFileA("ftest\\renamed\\b\\moved.txt", "ftest\\copy.txt", TRUE) && get("ftest\\copy.txt", back, sizeof(back)) == 13 && is_file("ftest\\renamed\\b\\moved.txt"), "CopyFile copies and keeps the source");
    SetLastError(0);
    check(!CopyFileA("ftest\\renamed\\b\\moved.txt", "ftest\\copy.txt", TRUE) && GetLastError() == ERROR_FILE_EXISTS, "copying onto a file with fail-if-exists is ERROR_FILE_EXISTS");
    check(CopyFileA("ftest\\big.bin", "ftest\\copy.txt", FALSE) && get("ftest\\copy.txt", back, sizeof(back)) == (long)sizeof(big) && memcmp(back, big, sizeof(big)) == 0, "copying over it replaces the content, all twenty thousand bytes");

    SetLastError(0);
    check(!DeleteFileA("ftest\\none.txt") && GetLastError() == ERROR_FILE_NOT_FOUND, "deleting a missing file is ERROR_FILE_NOT_FOUND");
    SetLastError(0);
    check(!DeleteFileA("ftest\\nodir\\x") && GetLastError() == ERROR_PATH_NOT_FOUND, "deleting under a missing directory is ERROR_PATH_NOT_FOUND");
    SetLastError(0);
    check(!DeleteFileA("ftest\\renamed") && GetLastError() == ERROR_ACCESS_DENIED && is_dir("ftest\\renamed"), "DeleteFile on a directory is ERROR_ACCESS_DENIED");
    SetLastError(0);
    check(!RemoveDirectoryA("ftest\\copy.txt") && GetLastError() == ERROR_DIRECTORY && is_file("ftest\\copy.txt"), "RemoveDirectory on a file is ERROR_DIRECTORY");
    SetLastError(0);
    check(!RemoveDirectoryA("ftest\\renamed") && GetLastError() == ERROR_DIR_NOT_EMPTY, "RemoveDirectory on a directory with files is ERROR_DIR_NOT_EMPTY");
    check(DeleteFileA("ftest\\renamed\\b\\moved.txt") && !is_file("ftest\\renamed\\b\\moved.txt") && RemoveDirectoryA("ftest\\renamed\\b") && RemoveDirectoryA("ftest\\renamed") && !is_dir("ftest\\renamed"), "empty directories are removed");

    h = CreateFileA("ftest\\temp.tmp", GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    check(h != INVALID_HANDLE_VALUE && is_file("ftest\\temp.tmp"), "a delete-on-close file exists while open");
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    check(!is_file("ftest\\temp.tmp"), "and is gone once closed");

    check(open_error("ftest", GENERIC_READ, OPEN_EXISTING, 0) == ERROR_ACCESS_DENIED, "opening a directory without backup semantics is ERROR_ACCESS_DENIED");
    h = CreateFileA("ftest", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    memset(&info, 0, sizeof(info));
    check(h != INVALID_HANDLE_VALUE && GetFileInformationByHandle(h, &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY), "a directory opens with backup semantics");
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

    {
        WIN32_FIND_DATAW wf;
        HANDLE s;
        h = CreateFileW(L"ftest\\w\u00e9.txt", GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        check(h != INVALID_HANDLE_VALUE && WriteFile(h, "w", 1, &got, NULL) && CloseHandle(h), "CreateFileW with a non-ASCII name");
        check(GetFileAttributesW(L"ftest\\w\u00e9.txt") != INVALID_FILE_ATTRIBUTES, "found by GetFileAttributesW");
        s = FindFirstFileW(L"ftest\\w*", &wf);
        check(s != INVALID_HANDLE_VALUE && same_wide(wf.cFileName, L"w\u00e9.txt"), "listed by FindFirstFileW");
        if (s != INVALID_HANDLE_VALUE) FindClose(s);
        check(MoveFileW(L"ftest\\w\u00e9.txt", L"ftest\\w2.txt") && DeleteFileW(L"ftest\\w2.txt") && CreateDirectoryW(L"ftest\\d\u00e9", NULL) && RemoveDirectoryW(L"ftest\\d\u00e9"), "the wide move, delete and directory calls");
    }

    check(is_file("A:\\files.exe") && is_dir("A:\\") && list("A:\\*.exe", NULL) >= 1, "A: is the read-only program directory");
    check(get("A:\\files.exe", back, sizeof(back)) > 1024 && back[0] == 'M' && back[1] == 'Z', "a program file reads from it");
    check(open_error("A:\\files.exe", GENERIC_WRITE, OPEN_EXISTING, 0) == ERROR_ACCESS_DENIED, "it cannot be written");
    SetLastError(0);
    check(!DeleteFileA("A:\\files.exe") && GetLastError() == ERROR_ACCESS_DENIED, "nor deleted");
    SetLastError(0);
    check(!CreateDirectoryA("A:\\made", NULL) && GetLastError() == ERROR_ACCESS_DENIED, "nor given new directories");
    check(open_error("D:\\x", GENERIC_READ, OPEN_EXISTING, 0) == ERROR_PATH_NOT_FOUND, "another drive is ERROR_PATH_NOT_FOUND");

    check(DeleteFileA("ftest\\pos.bin") && DeleteFileA("ftest\\big.bin") && DeleteFileA("ftest\\copy.txt"), "clean up");
    check(list("ftest\\*", NULL) == -1 && RemoveDirectoryA("ftest") && !is_dir("ftest"), "the test directory is empty and removed");

    printf("files done failures=%d\n", failures);
    return failures ? 1 : 4;
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

cat > "$OUT/threads.c" <<'C'
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <errno.h>

static int failures;

static void check(int ok, const char *what) {
    if (!ok) {
        failures++;
        printf("FAIL %s\n", what);
    }
}

static __thread int tls_value = 7;
static DWORD tls_slot;
static CRITICAL_SECTION section;
static volatile long guarded;
static HANDLE ping, pong;
static volatile long rounds_seen;
static HANDLE items_available;
static volatile long consumed;
static volatile long queue_total;
static SRWLOCK queue_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE queue_ready = CONDITION_VARIABLE_INIT;
static int queue_items[16];
static int queue_count;
static int queue_closed;
static long long queue_sum;
static HANDLE gate;
static volatile long ran_before_resume;
static volatile long started;

static DWORD WINAPI return_code(LPVOID p) { return (DWORD)(size_t)p + 5; }

static DWORD WINAPI count_guarded(LPVOID p) {
    int i;
    (void)p;
    for (i = 0; i < 5000; i++) {
        EnterCriticalSection(&section);
        EnterCriticalSection(&section);
        guarded = guarded + 1;
        LeaveCriticalSection(&section);
        LeaveCriticalSection(&section);
    }
    return 0;
}

static DWORD WINAPI ponger(LPVOID p) {
    int i;
    (void)p;
    for (i = 0; i < 200; i++) {
        WaitForSingleObject(ping, INFINITE);
        rounds_seen++;
        SetEvent(pong);
    }
    return 0;
}

static DWORD WINAPI consumer(LPVOID p) {
    (void)p;
    for (;;) {
        if (WaitForSingleObject(items_available, 2000) != WAIT_OBJECT_0) return 1;
        if (InterlockedDecrement(&queue_total) < 0) {
            InterlockedIncrement(&queue_total);
            return 0;
        }
        InterlockedIncrement(&consumed);
    }
}

static DWORD WINAPI thread_locals(LPVOID p) {
    int mine = (int)(size_t)p;
    DWORD id = GetCurrentThreadId();
    int ok = tls_value == 7;
    tls_value = 100 + mine;
    TlsSetValue(tls_slot, (LPVOID)(size_t)(200 + mine));
    SetLastError(300 + mine);
    errno = 400 + mine;
    Sleep(20);
    ok = ok && GetLastError() == (DWORD)(300 + mine) && tls_value == 100 + mine && (size_t)TlsGetValue(tls_slot) == (size_t)(200 + mine) && errno == 400 + mine && GetCurrentThreadId() == id;
    return ok ? 0 : 1;
}

static DWORD WINAPI producer(LPVOID p) {
    int n;
    (void)p;
    for (n = 1; n <= 300; n++) {
        AcquireSRWLockExclusive(&queue_lock);
        while (queue_count == 16) SleepConditionVariableSRW(&queue_ready, &queue_lock, INFINITE, 0);
        queue_items[queue_count++] = n;
        ReleaseSRWLockExclusive(&queue_lock);
        WakeAllConditionVariable(&queue_ready);
    }
    AcquireSRWLockExclusive(&queue_lock);
    queue_closed++;
    ReleaseSRWLockExclusive(&queue_lock);
    WakeAllConditionVariable(&queue_ready);
    return 0;
}

static DWORD WINAPI queue_taker(LPVOID p) {
    (void)p;
    for (;;) {
        AcquireSRWLockExclusive(&queue_lock);
        while (queue_count == 0 && queue_closed < 2) SleepConditionVariableSRW(&queue_ready, &queue_lock, INFINITE, 0);
        if (queue_count == 0) {
            ReleaseSRWLockExclusive(&queue_lock);
            return 0;
        }
        queue_sum += queue_items[--queue_count];
        ReleaseSRWLockExclusive(&queue_lock);
        WakeAllConditionVariable(&queue_ready);
    }
}

static DWORD WINAPI mark_started(LPVOID p) {
    (void)p;
    ran_before_resume = 1;
    started = 1;
    return 9;
}

static DWORD WINAPI wait_gate(LPVOID p) {
    (void)p;
    WaitForSingleObject(gate, INFINITE);
    return 0;
}

static DWORD WINAPI exits_early(LPVOID p) {
    (void)p;
    ExitThread(33);
    return 1;
}

static unsigned __stdcall via_crt(void *p) {
    if ((size_t)p == 1) _endthreadex(44);
    return 45;
}

static void __cdecl legacy_thread(void *p) { InterlockedExchange((volatile long *)p, 77); }

int main(void) {
    HANDLE h, many[3], held[8];
    DWORD code, id, main_id = GetCurrentThreadId();
    int i;
    unsigned crt_id;

    tls_slot = TlsAlloc();
    InitializeCriticalSection(&section);
    ping = CreateEventA(NULL, FALSE, FALSE, NULL);
    pong = CreateEventA(NULL, FALSE, FALSE, NULL);
    items_available = CreateSemaphoreA(NULL, 0, 1000, NULL);
    gate = CreateEventA(NULL, TRUE, FALSE, NULL);

    h = CreateThread(NULL, 0, return_code, (LPVOID)37, 0, &id);
    check(h != NULL && id != 0 && id != main_id, "CreateThread returns a handle and a distinct thread id");
    check(WaitForSingleObject(h, 5000) == WAIT_OBJECT_0 && GetExitCodeThread(h, &code) && code == 42, "a thread's return value is its exit code");
    check(GetThreadId(h) == id && CloseHandle(h), "GetThreadId names it and the handle closes");

    h = CreateThread(NULL, 0, count_guarded, NULL, 0, NULL);
    many[0] = h;
    many[1] = CreateThread(NULL, 0, count_guarded, NULL, 0, NULL);
    many[2] = CreateThread(NULL, 0, count_guarded, NULL, 0, NULL);
    check(WaitForMultipleObjects(3, many, TRUE, 10000) == WAIT_OBJECT_0 && guarded == 15000, "three threads under a critical section lose no update");
    for (i = 0; i < 3; i++) CloseHandle(many[i]);

    h = CreateThread(NULL, 0, ponger, NULL, 0, NULL);
    for (i = 0; i < 200; i++) {
        SetEvent(ping);
        if (WaitForSingleObject(pong, 5000) != WAIT_OBJECT_0) break;
    }
    check(i == 200 && rounds_seen == 200 && WaitForSingleObject(h, 5000) == WAIT_OBJECT_0, "two threads hand control back and forth through events 200 times");
    CloseHandle(h);

    queue_total = 60;
    many[0] = CreateThread(NULL, 0, consumer, NULL, 0, NULL);
    many[1] = CreateThread(NULL, 0, consumer, NULL, 0, NULL);
    many[2] = CreateThread(NULL, 0, consumer, NULL, 0, NULL);
    ReleaseSemaphore(items_available, 20, NULL);
    ReleaseSemaphore(items_available, 40, NULL);
    ReleaseSemaphore(items_available, 3, NULL);
    check(WaitForMultipleObjects(3, many, TRUE, 10000) == WAIT_OBJECT_0 && consumed == 60, "three semaphore consumers take exactly the units released");
    for (i = 0; i < 3; i++) CloseHandle(many[i]);

    for (i = 0; i < 3; i++) many[i] = CreateThread(NULL, 0, thread_locals, (LPVOID)(size_t)i, 0, NULL);
    check(WaitForMultipleObjects(3, many, TRUE, 10000) == WAIT_OBJECT_0, "the threads finished");
    for (i = 0; i < 3; i++) {
        GetExitCodeThread(many[i], &code);
        check(code == 0, "thread-local variable, TLS slot, last error and errno are separate per thread and start fresh");
        CloseHandle(many[i]);
    }
    check(tls_value == 7 && TlsGetValue(tls_slot) == NULL, "the main thread's own values are untouched");

    many[0] = CreateThread(NULL, 0, producer, NULL, 0, NULL);
    many[1] = CreateThread(NULL, 0, producer, NULL, 0, NULL);
    many[2] = CreateThread(NULL, 0, queue_taker, NULL, 0, NULL);
    held[0] = CreateThread(NULL, 0, queue_taker, NULL, 0, NULL);
    check(WaitForSingleObject(many[0], 10000) == WAIT_OBJECT_0 && WaitForSingleObject(many[1], 10000) == WAIT_OBJECT_0 && WaitForSingleObject(many[2], 10000) == WAIT_OBJECT_0 && WaitForSingleObject(held[0], 10000) == WAIT_OBJECT_0 && queue_sum == 2LL * 300 * 301 / 2,
          "SRW locks and condition variables move 600 items from two producers to two takers");
    for (i = 0; i < 3; i++) CloseHandle(many[i]);
    CloseHandle(held[0]);

    h = CreateThread(NULL, 0, mark_started, NULL, CREATE_SUSPENDED, NULL);
    Sleep(60);
    check(h != NULL && !ran_before_resume && GetExitCodeThread(h, &code) && code == STILL_ACTIVE, "a suspended thread does not run");
    check(ResumeThread(h) == 1 && WaitForSingleObject(h, 5000) == WAIT_OBJECT_0 && ran_before_resume && GetExitCodeThread(h, &code) && code == 9, "ResumeThread starts it");
    CloseHandle(h);

    h = CreateThread(NULL, 0, exits_early, NULL, 0, NULL);
    check(WaitForSingleObject(h, 5000) == WAIT_OBJECT_0 && GetExitCodeThread(h, &code) && code == 33, "ExitThread ends a thread with its code");
    CloseHandle(h);

    h = (HANDLE)_beginthreadex(NULL, 0, via_crt, (void *)1, 0, &crt_id);
    check(h != NULL && crt_id != 0 && WaitForSingleObject(h, 5000) == WAIT_OBJECT_0 && GetExitCodeThread(h, &code) && code == 44, "_endthreadex ends a thread with its code");
    CloseHandle(h);
    h = (HANDLE)_beginthreadex(NULL, 0, via_crt, (void *)0, 0, NULL);
    check(h != NULL && WaitForSingleObject(h, 5000) == WAIT_OBJECT_0 && GetExitCodeThread(h, &code) && code == 45, "_beginthreadex passes the return value on");
    CloseHandle(h);
    {
        volatile long flag = 0;
        uintptr_t legacy = _beginthread(legacy_thread, 0, (void *)&flag);
        check(legacy != (uintptr_t)-1, "_beginthread starts a thread");
        for (i = 0; i < 200 && flag == 0; i++) Sleep(10);
        check(flag == 77, "and it runs");
    }

    for (i = 0; i < 40; i++) {
        h = CreateThread(NULL, 0, return_code, (LPVOID)(size_t)i, 0, NULL);
        if (h == NULL) break;
        if (WaitForSingleObject(h, 5000) != WAIT_OBJECT_0 || !GetExitCodeThread(h, &code) || code != (DWORD)(i + 5)) break;
        CloseHandle(h);
    }
    check(i == 40, "forty threads one after another run, though the kernel only creates sixteen");

    for (i = 0; i < 7; i++) held[i] = CreateThread(NULL, 0, wait_gate, NULL, 0, NULL);
    for (i = 0; i < 7; i++) {
        if (held[i] == NULL) break;
    }
    check(i == 7, "seven threads can wait at once next to the main thread");
    h = CreateThread(NULL, 0, return_code, NULL, 0, NULL);
    check(h == NULL && GetLastError() == ERROR_NOT_ENOUGH_MEMORY, "an eighth extra thread is refused with ERROR_NOT_ENOUGH_MEMORY");
    SetEvent(gate);
    check(WaitForMultipleObjects(7, held, TRUE, 5000) == WAIT_OBJECT_0, "releasing the gate lets all seven finish");
    for (i = 0; i < 7; i++) CloseHandle(held[i]);
    h = CreateThread(NULL, 0, return_code, (LPVOID)1, 0, NULL);
    check(h != NULL && WaitForSingleObject(h, 5000) == WAIT_OBJECT_0, "a finished thread's place can be used again");
    CloseHandle(h);

    {
        HMODULE dll = LoadLibraryA("thrdll.dll");
        int (*attaches)(void) = dll ? (int (*)(void))GetProcAddress(dll, "thread_attaches") : NULL;
        int (*detaches)(void) = dll ? (int (*)(void))GetProcAddress(dll, "thread_detaches") : NULL;
        int (*local)(int) = dll ? (int (*)(int))GetProcAddress(dll, "dll_local") : NULL;
        check(dll != NULL && attaches && detaches && local, "a DLL with thread notifications and a thread-local variable loads");
        if (attaches && detaches && local) {
            int before = attaches();
            check(local(5) == 105 && local(1) == 106, "its thread-local variable starts at 100 for the main thread");
            h = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)local, (LPVOID)7, 0, NULL);
            WaitForSingleObject(h, 5000);
            GetExitCodeThread(h, &code);
            check(code == 107, "and starts again at 100 in a new thread");
            CloseHandle(h);
            check(attaches() == before + 1, "DLL_THREAD_ATTACH reached the DLL once for the new thread");
            for (i = 0; i < 100 && detaches() < 1; i++) Sleep(10);
            check(detaches() == 1, "DLL_THREAD_DETACH reached it when the thread ended");
            check(local(1) == 107, "the main thread's copy was not disturbed");
        }
    }

    printf("threads done failures=%d\n", failures);
    return failures ? 1 : 0;
}
C

cat > "$OUT/thrdll.c" <<'C'
#include <windows.h>

static volatile long attached;
static volatile long detached;
static __thread int local_value = 100;

__declspec(dllexport) int thread_attaches(void) { return (int)attached; }
__declspec(dllexport) int thread_detaches(void) { return (int)detached; }
__declspec(dllexport) DWORD WINAPI dll_local(LPVOID add) {
    local_value += (int)(size_t)add;
    return (DWORD)local_value;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    (void)h;
    (void)reserved;
    if (reason == DLL_THREAD_ATTACH) InterlockedIncrement(&attached);
    if (reason == DLL_THREAD_DETACH) InterlockedIncrement(&detached);
    return TRUE;
}
C

cat > "$OUT/argv.c" <<'C'
#include <windows.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    char text[256];
    char module[MAX_PATH];
    printf("argc=%d\n", argc);
    for (int i = 0; i < argc; i++) printf("argv[%d]=<%s>\n", i, argv[i]);
    printf("cmdline=<%s>\n", GetCommandLineA());
    DWORD n = GetModuleFileNameA(NULL, module, sizeof(module));
    printf("module=<%s>\n", n ? module : "?");
    n = GetEnvironmentVariableA("GREETING", text, sizeof(text));
    printf("GREETING=<%s>\n", n ? text : "unset");
    n = GetEnvironmentVariableA("SHELL", text, sizeof(text));
    printf("SHELL=<%s>\n", n ? text : "unset");
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    unsigned total = 0;
    char data[512];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(in, data + total, (DWORD)(sizeof(data) - 1 - total), &got, NULL) || got == 0) break;
        total += got;
        if (total >= sizeof(data) - 1) break;
    }
    data[total] = 0;
    printf("stdin %u bytes\n", total);
    if (total) printf("%s", data);
    return argc;
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
$CC -O1 -s -D__USE_MINGW_ANSI_STDIO=0 "$OUT/sysio.c" -o "$OUT/sysio.exe"
$CC -O1 -s -D__USE_MINGW_ANSI_STDIO=0 "$OUT/files.c" -o "$OUT/files.exe"
$CC -O1 -s -D__USE_MINGW_ANSI_STDIO=0 "$OUT/argv.c" -o "$OUT/argv.exe"
$CC -O1 -s -D__USE_MINGW_ANSI_STDIO=0 -static-libgcc -shared "$OUT/thrdll.c" -o "$OUT/thrdll.dll"
$CC -O1 -s -D__USE_MINGW_ANSI_STDIO=0 "$OUT/threads.c" -o "$OUT/threads.exe"
$CC -O2 -Wl,--no-dynamicbase -Wl,--disable-reloc-section "$OUT/stdio.c" -o "$OUT/stdio-fixed.exe"
$CC -O2 -s "$OUT/stdio.c" -o "$OUT/stdio-stripped.exe"
$CC -O2 -ffreestanding -fno-stack-protector -nostdlib -Wl,-e,entry -Wl,--subsystem,console "$OUT/k32.c" -o "$OUT/k32.exe" -lkernel32

if [ -x "$PEINFO" ]; then
    for f in stdio stdio-fixed stdio-stripped crt ucrt k32 dyn sysio argv files; do
        echo "================ $f.exe"
        "$PEINFO" "$OUT/$f.exe" || true
    done
fi
