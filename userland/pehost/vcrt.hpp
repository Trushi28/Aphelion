#pragma once
#include "msvcrt.hpp"

namespace pehost::vcrt {

struct ExceptionData {
    const char* what;
    u8 free_it;
};

inline u64 g_current_exception;
inline u64 g_current_context;
inline i32 g_processing_throw;
inline void* g_purecall_handler;
inline void* g_se_translator;

constexpr u32 STATUS_CXX_EXCEPTION = 0xE06D7363u;
constexpr u32 STATUS_STACK_BUFFER_OVERRUN = 0xC0000409u;

PEHOST_WINAPI inline void* memchr(const void* s, i32 c, u64 n) {
    const u8* p = static_cast<const u8*>(s);
    for (u64 i = 0; i < n; ++i) {
        if (p[i] == static_cast<u8>(c)) return const_cast<u8*>(p + i);
    }
    return nullptr;
}

PEHOST_WINAPI inline u16* wcschr(const u16* s, u16 c) {
    for (;; ++s) {
        if (*s == c) return const_cast<u16*>(s);
        if (*s == 0) return nullptr;
    }
}

PEHOST_WINAPI inline u16* wcsrchr(const u16* s, u16 c) {
    const u16* found = nullptr;
    for (;; ++s) {
        if (*s == c) found = s;
        if (*s == 0) return const_cast<u16*>(found);
    }
}

PEHOST_WINAPI inline u16* wcsstr(const u16* hay, const u16* needle) {
    u64 n = 0;
    while (needle[n]) ++n;
    if (n == 0) return const_cast<u16*>(hay);
    for (; *hay; ++hay) {
        u64 k = 0;
        while (k < n && hay[k] == needle[k]) ++k;
        if (k == n) return const_cast<u16*>(hay);
    }
    return nullptr;
}

PEHOST_WINAPI inline i32 __CxxFrameHandler3(void*, void*, void*, void*) { return 1; }

PEHOST_WINAPI inline i32 __CxxFrameHandler4(void*, void*, void*, void*) { return 1; }

PEHOST_WINAPI inline void _CxxThrowException(void*, void*) { detail::stop("a C++ exception was thrown and C++ exceptions are not supported", STATUS_CXX_EXCEPTION); }

PEHOST_WINAPI inline u64* __current_exception() { return &g_current_exception; }

PEHOST_WINAPI inline u64* __current_exception_context() { return &g_current_context; }

PEHOST_WINAPI inline i32* __processing_throw() { return &g_processing_throw; }

PEHOST_WINAPI inline u8 __uncaught_exception() { return 0; }

PEHOST_WINAPI inline i32 __uncaught_exceptions() { return 0; }

PEHOST_WINAPI inline void __std_terminate() { msvcrt::abort(); }

PEHOST_WINAPI inline void _purecall() { detail::stop("a pure virtual function was called", 3); }

PEHOST_WINAPI inline void* _get_purecall_handler() { return g_purecall_handler; }

PEHOST_WINAPI inline void* _set_purecall_handler(void* handler) {
    void* old = g_purecall_handler;
    g_purecall_handler = handler;
    return old;
}

PEHOST_WINAPI inline void __report_gsfailure(u64) { detail::stop("a stack buffer overrun was detected", STATUS_STACK_BUFFER_OVERRUN); }

PEHOST_WINAPI inline void* _set_se_translator(void* translator) {
    void* old = g_se_translator;
    g_se_translator = translator;
    return old;
}

PEHOST_WINAPI inline void __telemetry_main_invoke_trigger(void*) {}

PEHOST_WINAPI inline void __telemetry_main_return_trigger(void*) {}

PEHOST_WINAPI inline void __std_exception_copy(const ExceptionData* from, ExceptionData* to) {
    to->what = nullptr;
    to->free_it = 0;
    if (!from->what) return;
    if (!from->free_it) {
        to->what = from->what;
        return;
    }
    u64 n = 0;
    while (from->what[n]) ++n;
    char* copy = static_cast<char*>(g_world.heap.alloc(n + 1));
    if (!copy) return;
    for (u64 i = 0; i <= n; ++i) copy[i] = from->what[i];
    to->what = copy;
    to->free_it = 1;
}

PEHOST_WINAPI inline void __std_exception_destroy(ExceptionData* data) {
    if (data->free_it && data->what) g_world.heap.free(const_cast<char*>(data->what));
    data->what = nullptr;
    data->free_it = 0;
}

inline const char* decorated_name(const void* info) { return reinterpret_cast<const char*>(info) + 16; }

PEHOST_WINAPI inline i32 __std_type_info_compare(const void* a, const void* b) {
    if (a == b) return 0;
    const char* x = decorated_name(a);
    const char* y = decorated_name(b);
    while (*x && *x == *y) {
        ++x;
        ++y;
    }
    return static_cast<u8>(*x) < static_cast<u8>(*y) ? -1 : (*x == *y ? 0 : 1);
}

PEHOST_WINAPI inline u64 __std_type_info_hash(const void* info) {
    u64 h = 14695981039346656037ull;
    for (const char* p = decorated_name(info); *p; ++p) h = (h ^ static_cast<u8>(*p)) * 1099511628211ull;
    return h;
}

}

namespace pehost {

inline const ImportTable* vcrt_table() {
#define PEHOST_VC(name) { "vcruntime140.dll", #name, reinterpret_cast<void*>(&vcrt::name) }
#define PEHOST_VM(name) { "vcruntime140.dll", #name, reinterpret_cast<void*>(&msvcrt::name) }
    static const Import items[] = {
        PEHOST_VM(memcpy), PEHOST_VM(memmove), PEHOST_VM(memset), PEHOST_VM(memcmp), PEHOST_VM(strchr), PEHOST_VM(strrchr), PEHOST_VM(strstr),
        PEHOST_VM(__C_specific_handler), PEHOST_VC(memchr), PEHOST_VC(wcschr), PEHOST_VC(wcsrchr), PEHOST_VC(wcsstr),
        PEHOST_VC(__CxxFrameHandler3), PEHOST_VC(__CxxFrameHandler4), PEHOST_VC(_CxxThrowException), PEHOST_VC(__current_exception),
        PEHOST_VC(__current_exception_context), PEHOST_VC(__processing_throw), PEHOST_VC(__uncaught_exception), PEHOST_VC(__uncaught_exceptions),
        PEHOST_VC(__std_terminate), PEHOST_VC(_purecall), PEHOST_VC(_get_purecall_handler), PEHOST_VC(_set_purecall_handler),
        PEHOST_VC(__report_gsfailure), PEHOST_VC(_set_se_translator), PEHOST_VC(__telemetry_main_invoke_trigger),
        PEHOST_VC(__telemetry_main_return_trigger), PEHOST_VC(__std_exception_copy), PEHOST_VC(__std_exception_destroy),
        PEHOST_VC(__std_type_info_compare), PEHOST_VC(__std_type_info_hash),
    };
#undef PEHOST_VC
#undef PEHOST_VM
    static const ImportTable table{ items, sizeof(items) / sizeof(items[0]), msvcrt_table() };
    return &table;
}

}
