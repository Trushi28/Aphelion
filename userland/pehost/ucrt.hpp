#pragma once
#include "msvcrt.hpp"

namespace pehost::ucrt {

using msvcrt::File;

constexpr u64 PRINTF_STANDARD_SNPRINTF = 2;
constexpr i32 ARGV_NONE = 0;
constexpr i32 ARGV_UNEXPANDED = 1;
constexpr i32 ARGV_EXPANDED = 2;

struct OnexitTable {
    void** first;
    void** last;
    void** end;
};

inline i32 g_argc_value = 0;
inline char** g_argv_value = msvcrt::g_argv;
inline u16 g_wide_storage[msvcrt::ARG_STORAGE];
inline u16* g_wide_argv[msvcrt::MAX_ARGS];
inline u16** g_wide_argv_value = g_wide_argv;
inline bool g_wide_ready = false;
inline char** g_environ_value = msvcrt::g_environment;
inline u16* g_wide_environment[1] = { nullptr };
inline u16** g_wide_environ_value = g_wide_environment;
inline i32 g_new_mode = 0;
inline void* g_invalid_parameter = nullptr;
inline i32 g_daylight = 0;
inline i32 g_timezone = 0;
inline char g_zone_name[] = "UTC";
inline char* g_tzname[2] = { g_zone_name, g_zone_name };
inline void* g_quick_exit[msvcrt::MAX_ONEXIT];
inline u32 g_quick_exit_count = 0;

inline void reset() {
    msvcrt::reset();
    g_argc_value = 0;
    g_wide_ready = false;
    g_new_mode = 0;
    g_invalid_parameter = nullptr;
    g_quick_exit_count = 0;
}

namespace detail {

inline i32 narrow_arguments() {
    if (!msvcrt::g_args_ready) {
        if (!cmdline::parse(g_world.command_line, msvcrt::g_arg_storage, msvcrt::ARG_STORAGE, msvcrt::g_argv, msvcrt::MAX_ARGS,
                            &msvcrt::g_argc)) {
            return msvcrt::ERR_NOMEM;
        }
        msvcrt::g_args_ready = true;
    }
    g_argc_value = static_cast<i32>(msvcrt::g_argc);
    return 0;
}

inline i32 wide_arguments() {
    i32 r = narrow_arguments();
    if (r != 0) return r;
    if (g_wide_ready) return 0;
    u64 used = 0;
    for (u32 i = 0; i < msvcrt::g_argc; ++i) {
        const char* a = msvcrt::g_argv[i];
        if (used + 1 >= msvcrt::ARG_STORAGE) return msvcrt::ERR_NOMEM;
        i64 n = codec::utf8_to_utf16(reinterpret_cast<const u8*>(a), msvcrt::detail::length_of(a), g_wide_storage + used,
                                     msvcrt::ARG_STORAGE - used - 1, false);
        if (n < 0) return msvcrt::ERR_NOMEM;
        g_wide_argv[i] = g_wide_storage + used;
        used += static_cast<u64>(n);
        g_wide_storage[used++] = 0;
    }
    g_wide_argv[msvcrt::g_argc] = nullptr;
    g_wide_ready = true;
    return 0;
}

inline bool standard_snprintf(u64 options) { return (options & PRINTF_STANDARD_SNPRINTF) != 0; }

}

PEHOST_WINAPI inline File* __acrt_iob_func(u32 index) { return index < 3 ? &msvcrt::g_iob[index] : nullptr; }

PEHOST_WINAPI inline i32 __stdio_common_vfprintf(u64, File* fp, const char* f, void*, fmt::VaList ap) { return msvcrt::vfprintf(fp, f, ap); }

PEHOST_WINAPI inline i32 __stdio_common_vfwprintf(u64, File*, const u16*, void*, fmt::VaList) {
    msvcrt::g_errno = msvcrt::ERR_INVAL;
    return -1;
}

PEHOST_WINAPI inline i32 __stdio_common_vsprintf(u64 options, char* buffer, u64 count, const char* f, void*, fmt::VaList ap) {
    msvcrt::detail::BufferSink sink{ buffer, count, 0 };
    i64 r = fmt::format(sink, f, ap);
    if (r < 0) {
        msvcrt::g_errno = msvcrt::ERR_INVAL;
        if (count) buffer[0] = 0;
        return -1;
    }
    u64 total = static_cast<u64>(r);
    if (detail::standard_snprintf(options)) {
        if (count) buffer[total < count ? total : count - 1] = 0;
        return static_cast<i32>(total);
    }
    if (total < count) buffer[total] = 0;
    return total > count ? -1 : static_cast<i32>(total);
}

PEHOST_WINAPI inline i32 _configure_narrow_argv(i32 mode) {
    if (mode < ARGV_NONE || mode > ARGV_EXPANDED) return msvcrt::ERR_INVAL;
    if (mode == ARGV_NONE) {
        msvcrt::g_argc = 0;
        msvcrt::g_argv[0] = nullptr;
        msvcrt::g_args_ready = true;
        g_argc_value = 0;
        return 0;
    }
    return detail::narrow_arguments();
}

PEHOST_WINAPI inline i32 _configure_wide_argv(i32 mode) {
    if (mode < ARGV_NONE || mode > ARGV_EXPANDED) return msvcrt::ERR_INVAL;
    if (mode == ARGV_NONE) {
        msvcrt::g_argc = 0;
        msvcrt::g_argv[0] = nullptr;
        msvcrt::g_args_ready = true;
        g_wide_argv[0] = nullptr;
        g_wide_ready = true;
        g_argc_value = 0;
        return 0;
    }
    return detail::wide_arguments();
}

PEHOST_WINAPI inline i32* __p___argc() {
    detail::narrow_arguments();
    return &g_argc_value;
}

PEHOST_WINAPI inline char*** __p___argv() {
    detail::narrow_arguments();
    return &g_argv_value;
}

PEHOST_WINAPI inline u16*** __p___wargv() {
    detail::wide_arguments();
    return &g_wide_argv_value;
}

PEHOST_WINAPI inline char*** __p__environ() { return &g_environ_value; }

PEHOST_WINAPI inline u16*** __p__wenviron() { return &g_wide_environ_value; }

PEHOST_WINAPI inline char** _get_initial_narrow_environment() { return g_environ_value; }

PEHOST_WINAPI inline u16** _get_initial_wide_environment() { return g_wide_environ_value; }

PEHOST_WINAPI inline i32 _initialize_narrow_environment() { return 0; }

PEHOST_WINAPI inline i32 _initialize_wide_environment() { return 0; }

PEHOST_WINAPI inline i32* __p__fmode() { return &msvcrt::g_fmode; }

PEHOST_WINAPI inline i32* __p__commode() { return &msvcrt::g_commode; }

PEHOST_WINAPI inline i32 _set_fmode(i32 mode) {
    if (mode != 0 && mode != 0x4000 && mode != 0x8000) return msvcrt::ERR_INVAL;
    msvcrt::g_fmode = mode;
    return 0;
}

PEHOST_WINAPI inline i32 _get_fmode(i32* mode) {
    if (mode == nullptr) return msvcrt::ERR_INVAL;
    *mode = msvcrt::g_fmode;
    return 0;
}

PEHOST_WINAPI inline void _set_app_type(i32 type) { msvcrt::g_app_type = type; }

PEHOST_WINAPI inline i32 _set_new_mode(i32 mode) {
    if (mode != 0 && mode != 1) return -1;
    i32 old = g_new_mode;
    g_new_mode = mode;
    return old;
}

PEHOST_WINAPI inline void* _set_invalid_parameter_handler(void* handler) {
    void* old = g_invalid_parameter;
    g_invalid_parameter = handler;
    return old;
}

PEHOST_WINAPI inline i32 _configthreadlocale(i32) { return 0; }

PEHOST_WINAPI inline i32 _crt_atexit(void* fn) { return msvcrt::_onexit(fn) ? 0 : -1; }

PEHOST_WINAPI inline i32 _crt_at_quick_exit(void* fn) {
    if (g_quick_exit_count == msvcrt::MAX_ONEXIT) return -1;
    g_quick_exit[g_quick_exit_count++] = fn;
    return 0;
}

PEHOST_WINAPI inline void _c_exit() {}

PEHOST_WINAPI inline i32 _initialize_onexit_table(OnexitTable* table) {
    if (table == nullptr) return -1;
    table->first = table->last = table->end = nullptr;
    return 0;
}

PEHOST_WINAPI inline i32 _register_onexit_function(OnexitTable* table, void* fn) {
    if (table == nullptr) return -1;
    if (table->last == table->end) {
        u64 used = static_cast<u64>(table->last - table->first);
        u64 grown = used ? used * 2 : 8;
        void** moved = static_cast<void**>(g_world.heap.resize(table->first, grown * sizeof(void*)));
        if (moved == nullptr) return -1;
        table->first = moved;
        table->last = moved + used;
        table->end = moved + grown;
    }
    *table->last++ = fn;
    return 0;
}

PEHOST_WINAPI inline i32 _execute_onexit_table(OnexitTable* table) {
    if (table == nullptr) return -1;
    void** first = table->first;
    void** last = table->last;
    table->first = table->last = table->end = nullptr;
    while (last > first) {
        void* fn = *--last;
        if (fn) reinterpret_cast<void(PEHOST_WINAPI*)()>(fn)();
    }
    if (first) g_world.heap.free(first);
    return 0;
}

PEHOST_WINAPI inline void _fpreset() { asm volatile("fninit"); }

PEHOST_WINAPI inline i32* __daylight() { return &g_daylight; }

PEHOST_WINAPI inline i32* __timezone() { return &g_timezone; }

PEHOST_WINAPI inline char** __tzname() { return g_tzname; }

PEHOST_WINAPI inline void _tzset() {}

}

namespace pehost {

inline const ImportTable* all_imports() {
#define PEHOST_SHARED(name) { "ucrtbase.dll", #name, reinterpret_cast<void*>(&msvcrt::name) }
#define PEHOST_UCRT(name) { "ucrtbase.dll", #name, reinterpret_cast<void*>(&ucrt::name) }
    static const Import items[] = {
        PEHOST_SHARED(_errno), PEHOST_SHARED(fwrite), PEHOST_SHARED(fputc), PEHOST_SHARED(fputs), PEHOST_SHARED(puts),
        PEHOST_SHARED(putchar), PEHOST_SHARED(fflush), PEHOST_SHARED(malloc), PEHOST_SHARED(calloc), PEHOST_SHARED(free),
        PEHOST_SHARED(realloc), PEHOST_SHARED(memcpy), PEHOST_SHARED(memmove), PEHOST_SHARED(memset), PEHOST_SHARED(memcmp),
        PEHOST_SHARED(strlen), PEHOST_SHARED(wcslen), PEHOST_SHARED(strcmp), PEHOST_SHARED(strncmp), PEHOST_SHARED(strcpy),
        PEHOST_SHARED(strncpy), PEHOST_SHARED(strcat), PEHOST_SHARED(strchr), PEHOST_SHARED(strrchr), PEHOST_SHARED(strstr),
        PEHOST_SHARED(getenv), PEHOST_SHARED(strerror), PEHOST_SHARED(localeconv), PEHOST_SHARED(_onexit), PEHOST_SHARED(_cexit),
        PEHOST_SHARED(exit), PEHOST_SHARED(_exit), PEHOST_SHARED(abort), PEHOST_SHARED(signal), PEHOST_SHARED(_initterm),
        PEHOST_SHARED(_initterm_e), PEHOST_SHARED(__C_specific_handler), PEHOST_SHARED(__setusermatherr),
        PEHOST_UCRT(__acrt_iob_func), PEHOST_UCRT(__stdio_common_vfprintf), PEHOST_UCRT(__stdio_common_vfwprintf),
        PEHOST_UCRT(__stdio_common_vsprintf), PEHOST_UCRT(_configure_narrow_argv), PEHOST_UCRT(_configure_wide_argv),
        PEHOST_UCRT(__p___argc), PEHOST_UCRT(__p___argv), PEHOST_UCRT(__p___wargv), PEHOST_UCRT(__p__environ),
        PEHOST_UCRT(__p__wenviron), PEHOST_UCRT(_get_initial_narrow_environment), PEHOST_UCRT(_get_initial_wide_environment),
        PEHOST_UCRT(_initialize_narrow_environment), PEHOST_UCRT(_initialize_wide_environment), PEHOST_UCRT(__p__fmode),
        PEHOST_UCRT(__p__commode), PEHOST_UCRT(_set_fmode), PEHOST_UCRT(_get_fmode), PEHOST_UCRT(_set_app_type),
        PEHOST_UCRT(_set_new_mode), PEHOST_UCRT(_set_invalid_parameter_handler), PEHOST_UCRT(_configthreadlocale),
        PEHOST_UCRT(_crt_atexit), PEHOST_UCRT(_crt_at_quick_exit), PEHOST_UCRT(_c_exit), PEHOST_UCRT(_initialize_onexit_table),
        PEHOST_UCRT(_register_onexit_function), PEHOST_UCRT(_execute_onexit_table), PEHOST_UCRT(_fpreset), PEHOST_UCRT(__daylight),
        PEHOST_UCRT(__timezone), PEHOST_UCRT(__tzname), PEHOST_UCRT(_tzset),
    };
#undef PEHOST_SHARED
#undef PEHOST_UCRT
    static const ImportTable table{ items, sizeof(items) / sizeof(items[0]), msvcrt_table() };
    return &table;
}

}
