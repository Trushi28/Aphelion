#pragma once
#include "format.hpp"
#include "win32.hpp"

namespace pehost::msvcrt {

constexpr i32 ERR_BADF = 9;
constexpr i32 ERR_NOMEM = 12;
constexpr i32 ERR_INVAL = 22;
constexpr i32 ERR_RANGE = 34;
constexpr i32 EOF_VALUE = -1;
constexpr u32 MAX_ONEXIT = 32;
constexpr u32 MAX_SIGNAL = 32;
constexpr u32 ARG_STORAGE = 4096;
constexpr u32 MAX_ARGS = 64;
constexpr u32 EXIT_ABORT = 3;
constexpr u32 EXIT_RUNTIME = 255;

struct File {
    u64 words[6];
};

static_assert(sizeof(File) == 48);

struct LConv {
    const char* decimal_point;
    const char* thousands_sep;
    const char* grouping;
    const char* int_curr_symbol;
    const char* currency_symbol;
    const char* mon_decimal_point;
    const char* mon_thousands_sep;
    const char* mon_grouping;
    const char* positive_sign;
    const char* negative_sign;
    char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space, n_cs_precedes, n_sep_by_space, p_sign_posn, n_sign_posn;
    const u16* w_decimal_point;
    const u16* w_thousands_sep;
    const u16* w_int_curr_symbol;
    const u16* w_currency_symbol;
    const u16* w_mon_decimal_point;
    const u16* w_mon_thousands_sep;
    const u16* w_positive_sign;
    const u16* w_negative_sign;
};

inline File g_iob[3];
inline i32 g_errno = 0;
inline i32 g_fmode = 0;
inline i32 g_commode = 0;
inline i32 g_app_type = 0;
inline char** g_initenv = nullptr;
inline char* g_environment[1] = { nullptr };
inline char g_arg_storage[ARG_STORAGE];
inline char* g_argv[MAX_ARGS];
inline u32 g_argc = 0;
inline bool g_args_ready = false;
inline void* g_onexit[MAX_ONEXIT];
inline u32 g_onexit_count = 0;
inline void* g_signals[MAX_SIGNAL];
inline void* g_matherr = nullptr;
inline const u16 g_wide_dot[] = { '.', 0 };
inline const u16 g_wide_empty[] = { 0 };
inline LConv g_lconv = {
    ".", "", "", "", "", "", "", "", "", "",
    127, 127, 127, 127, 127, 127, 127, 127,
    g_wide_dot, g_wide_empty, g_wide_empty, g_wide_empty, g_wide_empty, g_wide_empty, g_wide_empty, g_wide_empty,
};

inline void reset() {
    g_errno = 0;
    g_fmode = 0;
    g_commode = 0;
    g_app_type = 0;
    g_initenv = nullptr;
    g_argc = 0;
    g_args_ready = false;
    g_onexit_count = 0;
    g_matherr = nullptr;
    for (u32 i = 0; i < MAX_ONEXIT; ++i) g_onexit[i] = nullptr;
    for (u32 i = 0; i < MAX_SIGNAL; ++i) g_signals[i] = nullptr;
}

namespace detail {

inline u32 stream_of(const File* fp) {
    if (fp == &g_iob[1]) return 1;
    if (fp == &g_iob[2]) return 2;
    return 0;
}

inline void console_write(u32 stream, const char* data, u64 length) {
    while (length) {
        u32 chunk = length > 4096 ? 4096 : static_cast<u32>(length);
        g_runtime->write_console(stream, data, chunk);
        data += chunk;
        length -= chunk;
    }
}

struct ConsoleSink {
    u32 stream;
    void put(const char* s, u64 n) { console_write(stream, s, n); }
};

struct BufferSink {
    char* at;
    u64 capacity;
    u64 used;
    void put(const char* s, u64 n) {
        for (u64 i = 0; i < n && used < capacity; ++i) at[used++] = s[i];
    }
};

inline u64 length_of(const char* s) {
    u64 n = 0;
    while (s[n]) ++n;
    return n;
}

inline void run_exit_handlers() {
    while (g_onexit_count) {
        void* fn = g_onexit[--g_onexit_count];
        reinterpret_cast<void(PEHOST_WINAPI*)()>(fn)();
    }
}

inline void say(const char* text) { console_write(2, text, length_of(text)); }

}

PEHOST_WINAPI inline File* __iob_func() { return g_iob; }

PEHOST_WINAPI inline i32* _errno() { return &g_errno; }

PEHOST_WINAPI inline u64 fwrite(const void* data, u64 size, u64 count, File* fp) {
    u32 stream = detail::stream_of(fp);
    if (stream == 0) {
        g_errno = ERR_BADF;
        return 0;
    }
    if (size == 0 || count == 0) return 0;
    if (count > ~0ull / size) {
        g_errno = ERR_INVAL;
        return 0;
    }
    detail::console_write(stream, static_cast<const char*>(data), size * count);
    return count;
}

PEHOST_WINAPI inline i32 fputc(i32 c, File* fp) {
    u32 stream = detail::stream_of(fp);
    if (stream == 0) {
        g_errno = ERR_BADF;
        return EOF_VALUE;
    }
    char ch = static_cast<char>(c);
    detail::console_write(stream, &ch, 1);
    return static_cast<u8>(ch);
}

PEHOST_WINAPI inline i32 fputs(const char* s, File* fp) {
    u32 stream = detail::stream_of(fp);
    if (stream == 0) {
        g_errno = ERR_BADF;
        return EOF_VALUE;
    }
    detail::console_write(stream, s, detail::length_of(s));
    return 0;
}

PEHOST_WINAPI inline i32 puts(const char* s) {
    detail::console_write(1, s, detail::length_of(s));
    detail::console_write(1, "\n", 1);
    return 0;
}

PEHOST_WINAPI inline i32 putchar(i32 c) {
    char ch = static_cast<char>(c);
    detail::console_write(1, &ch, 1);
    return static_cast<u8>(ch);
}

PEHOST_WINAPI inline i32 fflush(File* fp) {
    if (fp == nullptr || detail::stream_of(fp) != 0 || fp == &g_iob[0]) return 0;
    g_errno = ERR_BADF;
    return EOF_VALUE;
}

PEHOST_WINAPI inline i32 vfprintf(File* fp, const char* f, fmt::VaList ap) {
    u32 stream = detail::stream_of(fp);
    if (stream == 0) {
        g_errno = ERR_BADF;
        return -1;
    }
    detail::ConsoleSink sink{ stream };
    i64 r = fmt::format(sink, f, ap);
    if (r < 0) g_errno = ERR_INVAL;
    return static_cast<i32>(r);
}

PEHOST_WINAPI inline i32 vprintf(const char* f, fmt::VaList ap) { return vfprintf(&g_iob[1], f, ap); }

PEHOST_WINAPI inline i32 printf(const char* f, ...) {
    fmt::VaList ap;
    __builtin_ms_va_start(ap, f);
    i32 r = vprintf(f, ap);
    __builtin_ms_va_end(ap);
    return r;
}

PEHOST_WINAPI inline i32 fprintf(File* fp, const char* f, ...) {
    fmt::VaList ap;
    __builtin_ms_va_start(ap, f);
    i32 r = vfprintf(fp, f, ap);
    __builtin_ms_va_end(ap);
    return r;
}

PEHOST_WINAPI inline i32 _vsnprintf(char* buffer, u64 count, const char* f, fmt::VaList ap) {
    detail::BufferSink sink{ buffer, count, 0 };
    i64 r = fmt::format(sink, f, ap);
    if (r < 0) {
        g_errno = ERR_INVAL;
        return -1;
    }
    if (static_cast<u64>(r) < count) buffer[r] = 0;
    return static_cast<u64>(r) > count ? -1 : static_cast<i32>(r);
}

PEHOST_WINAPI inline i32 _snprintf(char* buffer, u64 count, const char* f, ...) {
    fmt::VaList ap;
    __builtin_ms_va_start(ap, f);
    i32 r = _vsnprintf(buffer, count, f, ap);
    __builtin_ms_va_end(ap);
    return r;
}

PEHOST_WINAPI inline i32 vsprintf(char* buffer, const char* f, fmt::VaList ap) {
    detail::BufferSink sink{ buffer, ~0ull >> 1, 0 };
    i64 r = fmt::format(sink, f, ap);
    if (r < 0) {
        g_errno = ERR_INVAL;
        return -1;
    }
    buffer[r] = 0;
    return static_cast<i32>(r);
}

PEHOST_WINAPI inline i32 sprintf(char* buffer, const char* f, ...) {
    fmt::VaList ap;
    __builtin_ms_va_start(ap, f);
    i32 r = vsprintf(buffer, f, ap);
    __builtin_ms_va_end(ap);
    return r;
}

PEHOST_WINAPI inline void* malloc(u64 bytes) {
    void* p = g_world.heap.alloc(bytes);
    if (!p) g_errno = ERR_NOMEM;
    return p;
}

PEHOST_WINAPI inline void* calloc(u64 count, u64 size) {
    if (size != 0 && count > ~0ull / size) {
        g_errno = ERR_NOMEM;
        return nullptr;
    }
    u64 bytes = count * size;
    u8* p = static_cast<u8*>(g_world.heap.alloc(bytes));
    if (!p) {
        g_errno = ERR_NOMEM;
        return nullptr;
    }
    for (u64 i = 0; i < bytes; ++i) p[i] = 0;
    return p;
}

PEHOST_WINAPI inline void free(void* p) { g_world.heap.free(p); }

PEHOST_WINAPI inline void* realloc(void* p, u64 bytes) {
    if (p && bytes == 0) {
        g_world.heap.free(p);
        return nullptr;
    }
    void* q = g_world.heap.resize(p, bytes);
    if (!q) g_errno = ERR_NOMEM;
    return q;
}

PEHOST_WINAPI inline void* memcpy(void* to, const void* from, u64 n) {
    u8* d = static_cast<u8*>(to);
    const u8* s = static_cast<const u8*>(from);
    for (u64 i = 0; i < n; ++i) d[i] = s[i];
    return to;
}

PEHOST_WINAPI inline void* memmove(void* to, const void* from, u64 n) {
    u8* d = static_cast<u8*>(to);
    const u8* s = static_cast<const u8*>(from);
    if (d < s) {
        for (u64 i = 0; i < n; ++i) d[i] = s[i];
    } else {
        for (u64 i = n; i > 0; --i) d[i - 1] = s[i - 1];
    }
    return to;
}

PEHOST_WINAPI inline void* memset(void* to, i32 value, u64 n) {
    u8* d = static_cast<u8*>(to);
    for (u64 i = 0; i < n; ++i) d[i] = static_cast<u8>(value);
    return to;
}

PEHOST_WINAPI inline i32 memcmp(const void* a, const void* b, u64 n) {
    const u8* x = static_cast<const u8*>(a);
    const u8* y = static_cast<const u8*>(b);
    for (u64 i = 0; i < n; ++i) {
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    }
    return 0;
}

PEHOST_WINAPI inline u64 strlen(const char* s) { return detail::length_of(s); }

PEHOST_WINAPI inline u64 wcslen(const u16* s) {
    u64 n = 0;
    while (s[n]) ++n;
    return n;
}

PEHOST_WINAPI inline i32 strcmp(const char* a, const char* b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    u8 x = static_cast<u8>(*a), y = static_cast<u8>(*b);
    return x == y ? 0 : (x < y ? -1 : 1);
}

PEHOST_WINAPI inline i32 strncmp(const char* a, const char* b, u64 n) {
    for (u64 i = 0; i < n; ++i) {
        u8 x = static_cast<u8>(a[i]), y = static_cast<u8>(b[i]);
        if (x != y) return x < y ? -1 : 1;
        if (x == 0) return 0;
    }
    return 0;
}

PEHOST_WINAPI inline char* strcpy(char* to, const char* from) {
    u64 i = 0;
    do {
        to[i] = from[i];
    } while (from[i++]);
    return to;
}

PEHOST_WINAPI inline char* strncpy(char* to, const char* from, u64 n) {
    u64 i = 0;
    for (; i < n && from[i]; ++i) to[i] = from[i];
    for (; i < n; ++i) to[i] = 0;
    return to;
}

PEHOST_WINAPI inline char* strcat(char* to, const char* from) {
    strcpy(to + detail::length_of(to), from);
    return to;
}

PEHOST_WINAPI inline char* strchr(const char* s, i32 c) {
    char want = static_cast<char>(c);
    for (;; ++s) {
        if (*s == want) return const_cast<char*>(s);
        if (*s == 0) return nullptr;
    }
}

PEHOST_WINAPI inline char* strrchr(const char* s, i32 c) {
    char want = static_cast<char>(c);
    const char* found = nullptr;
    for (;; ++s) {
        if (*s == want) found = s;
        if (*s == 0) return const_cast<char*>(found);
    }
}

PEHOST_WINAPI inline char* strstr(const char* hay, const char* needle) {
    u64 n = detail::length_of(needle);
    if (n == 0) return const_cast<char*>(hay);
    for (; *hay; ++hay) {
        u64 k = 0;
        while (k < n && hay[k] == needle[k]) ++k;
        if (k == n) return const_cast<char*>(hay);
    }
    return nullptr;
}

PEHOST_WINAPI inline char* getenv(const char*) { return nullptr; }

PEHOST_WINAPI inline const char* strerror(i32 code) {
    switch (code) {
        case 0: return "No error";
        case 1: return "Operation not permitted";
        case 2: return "No such file or directory";
        case ERR_BADF: return "Bad file descriptor";
        case ERR_NOMEM: return "Not enough space";
        case ERR_INVAL: return "Invalid argument";
        case ERR_RANGE: return "Result too large";
        default: return "Unknown error";
    }
}

PEHOST_WINAPI inline LConv* localeconv() { return &g_lconv; }

PEHOST_WINAPI inline void* _onexit(void* fn) {
    if (g_onexit_count == MAX_ONEXIT) return nullptr;
    g_onexit[g_onexit_count++] = fn;
    return fn;
}

PEHOST_WINAPI inline i32 atexit(void* fn) { return _onexit(fn) ? 0 : -1; }

PEHOST_WINAPI inline void _cexit() { detail::run_exit_handlers(); }

PEHOST_WINAPI inline void exit(i32 code) {
    detail::run_exit_handlers();
    g_runtime->exit_process(static_cast<u32>(code));
    for (;;) asm volatile("ud2");
}

PEHOST_WINAPI inline void _exit(i32 code) {
    g_runtime->exit_process(static_cast<u32>(code));
    for (;;) asm volatile("ud2");
}

PEHOST_WINAPI inline void abort() {
    detail::say("\nThis application has requested the Runtime to terminate it in an unusual way.\nPlease contact the application's support team for more information.\n");
    g_runtime->exit_process(EXIT_ABORT);
    for (;;) asm volatile("ud2");
}

PEHOST_WINAPI inline void _amsg_exit(i32 code) {
    char text[40];
    const char* head = "\nruntime error R";
    u32 n = 0;
    while (head[n]) {
        text[n] = head[n];
        ++n;
    }
    u32 number = 6000 + static_cast<u32>(code) % 1000;
    for (u32 div = 1000; div; div /= 10) text[n++] = static_cast<char>('0' + number / div % 10);
    text[n++] = '\r';
    text[n++] = '\n';
    text[n] = 0;
    detail::say(text);
    g_runtime->exit_process(EXIT_RUNTIME);
    for (;;) asm volatile("ud2");
}

PEHOST_WINAPI inline void* signal(i32 sig, void* handler) {
    bool valid = sig == 2 || sig == 4 || sig == 8 || sig == 11 || sig == 15 || sig == 21 || sig == 22;
    if (!valid) {
        g_errno = ERR_INVAL;
        return reinterpret_cast<void*>(~0ull);
    }
    void* old = g_signals[sig];
    g_signals[sig] = handler;
    return old;
}

PEHOST_WINAPI inline void __set_app_type(i32 type) { g_app_type = type; }

PEHOST_WINAPI inline void* __setusermatherr(void* handler) {
    void* old = g_matherr;
    g_matherr = handler;
    return old;
}

PEHOST_WINAPI inline void _initterm(void** first, void** last) {
    for (void** p = first; p < last; ++p) {
        if (*p) reinterpret_cast<void(PEHOST_WINAPI*)()>(*p)();
    }
}

PEHOST_WINAPI inline i32 _initterm_e(void** first, void** last) {
    for (void** p = first; p < last; ++p) {
        if (!*p) continue;
        i32 r = reinterpret_cast<i32(PEHOST_WINAPI*)()>(*p)();
        if (r != 0) return r;
    }
    return 0;
}

PEHOST_WINAPI inline i32 __getmainargs(i32* argc, char*** argv, char*** envp, i32, void*) {
    if (!g_args_ready) {
        if (!cmdline::parse(g_world.command_line, g_arg_storage, ARG_STORAGE, g_argv, MAX_ARGS, &g_argc)) return -1;
        g_args_ready = true;
    }
    *argc = static_cast<i32>(g_argc);
    *argv = g_argv;
    *envp = g_environment;
    return 0;
}

PEHOST_WINAPI inline void _lock(i32) {}

PEHOST_WINAPI inline void _unlock(i32) {}

PEHOST_WINAPI inline u32 ___lc_codepage_func() { return 0; }

PEHOST_WINAPI inline i32 ___mb_cur_max_func() { return 1; }

PEHOST_WINAPI inline i32 __C_specific_handler(void*, void*, void*, void*) { return 1; }

}

namespace pehost {

inline const ImportTable* all_imports() {
#define PEHOST_FN(name) { "msvcrt.dll", #name, reinterpret_cast<void*>(&msvcrt::name) }
#define PEHOST_DATA(name, object) { "msvcrt.dll", #name, reinterpret_cast<void*>(&msvcrt::object) }
    static const Import items[] = {
        PEHOST_FN(__iob_func), PEHOST_FN(_errno), PEHOST_FN(fwrite), PEHOST_FN(fputc), PEHOST_FN(fputs), PEHOST_FN(puts),
        PEHOST_FN(putchar), PEHOST_FN(fflush), PEHOST_FN(vfprintf), PEHOST_FN(vprintf), PEHOST_FN(printf), PEHOST_FN(fprintf),
        PEHOST_FN(_vsnprintf), PEHOST_FN(_snprintf), PEHOST_FN(vsprintf), PEHOST_FN(sprintf), PEHOST_FN(malloc), PEHOST_FN(calloc),
        PEHOST_FN(free), PEHOST_FN(realloc), PEHOST_FN(memcpy), PEHOST_FN(memmove), PEHOST_FN(memset), PEHOST_FN(memcmp),
        PEHOST_FN(strlen), PEHOST_FN(wcslen), PEHOST_FN(strcmp), PEHOST_FN(strncmp), PEHOST_FN(strcpy), PEHOST_FN(strncpy),
        PEHOST_FN(strcat), PEHOST_FN(strchr), PEHOST_FN(strrchr), PEHOST_FN(strstr), PEHOST_FN(getenv), PEHOST_FN(strerror),
        PEHOST_FN(localeconv), PEHOST_FN(_onexit), PEHOST_FN(atexit), PEHOST_FN(_cexit), PEHOST_FN(exit), PEHOST_FN(_exit),
        PEHOST_FN(abort), PEHOST_FN(_amsg_exit), PEHOST_FN(signal), PEHOST_FN(__set_app_type), PEHOST_FN(__setusermatherr),
        PEHOST_FN(_initterm), PEHOST_FN(_initterm_e), PEHOST_FN(__getmainargs), PEHOST_FN(_lock), PEHOST_FN(_unlock),
        PEHOST_FN(___lc_codepage_func), PEHOST_FN(___mb_cur_max_func), PEHOST_FN(__C_specific_handler),
        PEHOST_DATA(_fmode, g_fmode), PEHOST_DATA(_commode, g_commode), PEHOST_DATA(__initenv, g_initenv),
    };
#undef PEHOST_FN
#undef PEHOST_DATA
    static const ImportTable table{ items, sizeof(items) / sizeof(items[0]), kernel32_table() };
    return &table;
}

}
