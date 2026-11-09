#pragma once
#include "cmdline.hpp"
#include "codec.hpp"
#include "heap.hpp"
#include "host.hpp"

namespace pehost {

struct Runtime {
    void (*exit_process)(u32 code) = nullptr;
    i64 (*write_console)(u32 stream, const void* data, u32 length) = nullptr;
    u8* (*reserve)(u64 bytes) = nullptr;
    bool (*protect)(u8* at, u64 bytes, u32 prot) = nullptr;
    void (*sleep_ms)(u32 milliseconds) = nullptr;
};

inline Runtime* g_runtime = nullptr;

constexpr u32 ERROR_NOT_SUPPORTED = 50;
constexpr u32 ERROR_INVALID_HANDLE = 6;
constexpr u32 ERROR_NOT_ENOUGH_MEMORY = 8;
constexpr u32 ERROR_INVALID_PARAMETER = 87;
constexpr u32 ERROR_INSUFFICIENT_BUFFER = 122;
constexpr u32 ERROR_PROC_NOT_FOUND = 127;
constexpr u32 ERROR_INVALID_FLAGS = 1004;
constexpr u32 ERROR_NO_UNICODE_TRANSLATION = 1113;
constexpr u32 ERROR_MOD_NOT_FOUND = 126;
constexpr u32 STD_INPUT = 0xFFFFFFF6u;
constexpr u32 STD_OUTPUT = 0xFFFFFFF5u;
constexpr u32 STD_ERROR = 0xFFFFFFF4u;
constexpr u64 HANDLE_STDIN = 4;
constexpr u64 HANDLE_STDOUT = 8;
constexpr u64 HANDLE_STDERR = 12;
constexpr u64 INVALID_HANDLE = ~0ull;
constexpr u64 CURRENT_PROCESS = ~0ull;
constexpr u64 CURRENT_THREAD = ~1ull;
constexpr u32 TLS_OUT_OF_INDEXES = 0xFFFFFFFFu;
constexpr u32 HEAP_ZERO_MEMORY = 8;
constexpr u32 HEAP_REALLOC_IN_PLACE_ONLY = 16;
constexpr u32 CODEPAGE_ACP = 0;
constexpr u32 CODEPAGE_OEM = 1;
constexpr u32 CODEPAGE_THREAD = 3;
constexpr u32 CODEPAGE_UTF8 = 65001;
constexpr u32 MB_ERR_INVALID_CHARS = 8;
constexpr u32 MB_PRECOMPOSED = 1;
constexpr u32 WC_ERR_INVALID_CHARS = 0x80;
constexpr u32 MAX_IMAGE_PAGES = 4096;
constexpr u32 MAX_MODULE_PATH = 260;
constexpr u32 MAX_COMMAND_LINE = 1024;

struct CriticalSection {
    u64 debug_info;
    i32 lock_count;
    i32 recursion_count;
    u64 owning_thread;
    u64 lock_semaphore;
    u64 spin_count;
};

static_assert(sizeof(CriticalSection) == 40);

struct MemoryBasicInformation {
    u64 base_address;
    u64 allocation_base;
    u32 allocation_protect;
    u16 partition_id;
    u16 pad;
    u64 region_size;
    u32 state;
    u32 protect;
    u32 type;
    u32 pad2;
};

static_assert(sizeof(MemoryBasicInformation) == 48);

struct World {
    Heap heap;
    const ImportTable* imports = nullptr;
    u8* image = nullptr;
    u32 image_pages = 0;
    u8 page_prot[MAX_IMAGE_PAGES] = {};
    u64 tls_used = 0;
    void* unhandled_filter = nullptr;
    char command_line[MAX_COMMAND_LINE] = {};
    u16 command_line_wide[MAX_COMMAND_LINE] = {};
    char module_path[MAX_MODULE_PATH] = {};
};

inline World g_world;

struct Config {
    const char* command_line;
    const char* module_path;
};

namespace detail {

inline void copy_text(char* to, u32 cap, const char* from) {
    u32 i = 0;
    while (from[i] && i + 1 < cap) {
        to[i] = from[i];
        ++i;
    }
    to[i] = 0;
}

inline u32 text_length(const char* s) {
    u32 n = 0;
    while (s[n]) ++n;
    return n;
}

inline u8* heap_map(u64 bytes) { return g_runtime->reserve(bytes); }

inline u32 windows_protect(u32 prot) {
    bool r = prot & pe::PROT_READ, w = prot & pe::PROT_WRITE, x = prot & pe::PROT_EXEC;
    if (x) return w ? 0x40 : (r ? 0x20 : 0x10);
    if (w) return 4;
    return r ? 2 : 1;
}

inline bool native_protect(u32 windows, u32* prot) {
    switch (windows) {
        case 1: *prot = 0; return true;
        case 2: *prot = pe::PROT_READ; return true;
        case 4:
        case 8: *prot = pe::PROT_READ | pe::PROT_WRITE; return true;
        case 0x10:
        case 0x20: *prot = pe::PROT_READ | pe::PROT_EXEC; return true;
        default: return false;
    }
}

inline bool code_page_ok(u32 cp) { return cp == CODEPAGE_ACP || cp == CODEPAGE_OEM || cp == CODEPAGE_THREAD || cp == CODEPAGE_UTF8; }

inline Teb* current_teb() {
    Teb* t;
    asm volatile("movq %%gs:0x30, %0" : "=r"(t));
    return t;
}

inline u64 current_thread_id() { return current_teb()->client_thread; }

inline const char* const MODULE_NAMES[] = { "kernel32.dll", "msvcrt.dll", "ucrtbase.dll" };
inline u8 MODULE_TOKENS[sizeof(MODULE_NAMES) / sizeof(MODULE_NAMES[0])][16];

inline const char* module_name(u64 token) {
    for (u32 i = 0; i < sizeof(MODULE_NAMES) / sizeof(MODULE_NAMES[0]); ++i) {
        if (token == reinterpret_cast<u64>(&MODULE_TOKENS[i][0])) return MODULE_NAMES[i];
    }
    return nullptr;
}

inline u64 module_token(const char* name) {
    const char* base = name;
    for (const char* p = name; *p; ++p) {
        if (*p == '\\' || *p == '/') base = p + 1;
    }
    char wanted[48];
    u32 n = 0;
    while (base[n] && n + 5 < sizeof(wanted)) {
        wanted[n] = fold(base[n]);
        ++n;
    }
    if (base[n] || n == 0) return 0;
    wanted[n] = 0;
    bool dotted = false;
    for (u32 i = 0; i < n; ++i) {
        if (wanted[i] == '.') dotted = true;
    }
    if (!dotted) {
        wanted[n++] = '.';
        wanted[n++] = 'd';
        wanted[n++] = 'l';
        wanted[n++] = 'l';
        wanted[n] = 0;
    }
    const char* home = pe::modules::canonical(wanted);
    for (u32 i = 0; i < sizeof(MODULE_NAMES) / sizeof(MODULE_NAMES[0]); ++i) {
        if (same_name(MODULE_NAMES[i], home)) return reinterpret_cast<u64>(&MODULE_TOKENS[i][0]);
    }
    return 0;
}

}

inline void set_last_error(u32 code) { asm volatile("movl %0, %%gs:0x68" : : "r"(code) : "memory"); }

inline u32 last_error() {
    u32 code;
    asm volatile("movl %%gs:0x68, %0" : "=r"(code));
    return code;
}

inline void attach(Prepared& run, const ImportTable* imports, const Config& config) {
    World& w = g_world;
    w.heap.init(&detail::heap_map);
    w.imports = imports;
    w.image = run.base;
    u64 pages = run.image.size_of_image / PAGE;
    w.image_pages = pages > MAX_IMAGE_PAGES ? MAX_IMAGE_PAGES : static_cast<u32>(pages);
    for (u32 i = 0; i < MAX_IMAGE_PAGES; ++i) w.page_prot[i] = 0;
    pe::Run runs[pe::MAX_RUNS];
    u32 count = pe::plan(run.image, runs, pe::MAX_RUNS);
    for (u32 i = 0; i < count; ++i) {
        for (u32 k = 0; k < runs[i].pages; ++k) {
            u32 page = runs[i].first_page + k;
            if (page < w.image_pages) w.page_prot[page] = static_cast<u8>(runs[i].prot);
        }
    }
    w.tls_used = 0;
    w.unhandled_filter = nullptr;
    detail::copy_text(w.command_line, MAX_COMMAND_LINE, config.command_line);
    detail::copy_text(w.module_path, MAX_MODULE_PATH, config.module_path);
    i64 wide = codec::utf8_to_utf16(reinterpret_cast<const u8*>(w.command_line), detail::text_length(w.command_line), w.command_line_wide,
                                    MAX_COMMAND_LINE - 1, false);
    w.command_line_wide[wide > 0 ? wide : 0] = 0;
    run.peb->process_heap = reinterpret_cast<u64>(&w.heap);
}

}

#define PEHOST_WINAPI __attribute__((ms_abi))

namespace pehost::win32 {

PEHOST_WINAPI inline void ExitProcess(u32 code) {
    g_runtime->exit_process(code);
    for (;;) asm volatile("ud2");
}

PEHOST_WINAPI inline i32 TerminateProcess(u64 process, u32 code) {
    if (process != CURRENT_PROCESS) {
        set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    ExitProcess(code);
    return 0;
}

PEHOST_WINAPI inline u64 GetStdHandle(u32 which) {
    switch (which) {
        case STD_INPUT: return HANDLE_STDIN;
        case STD_OUTPUT: return HANDLE_STDOUT;
        case STD_ERROR: return HANDLE_STDERR;
        default:
            set_last_error(ERROR_INVALID_HANDLE);
            return INVALID_HANDLE;
    }
}

PEHOST_WINAPI inline i32 WriteFile(u64 handle, const void* data, u32 length, u32* written, void* overlapped) {
    if (written) *written = 0;
    if (handle != HANDLE_STDOUT && handle != HANDLE_STDERR) {
        set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    if (overlapped != nullptr) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    g_runtime->write_console(handle == HANDLE_STDERR ? 2 : 1, data, length);
    if (written) *written = length;
    return 1;
}

PEHOST_WINAPI inline u32 GetLastError() { return last_error(); }

PEHOST_WINAPI inline void SetLastError(u32 code) { set_last_error(code); }

PEHOST_WINAPI inline u64 GetProcessHeap() { return reinterpret_cast<u64>(&g_world.heap); }

PEHOST_WINAPI inline void* HeapAlloc(u64 heap, u32 flags, u64 bytes) {
    if (heap != GetProcessHeap()) {
        set_last_error(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    void* p = g_world.heap.alloc(bytes);
    if (!p) {
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    if (flags & HEAP_ZERO_MEMORY) {
        u8* b = static_cast<u8*>(p);
        u64 n = g_world.heap.usable(p);
        for (u64 i = 0; i < n; ++i) b[i] = 0;
    }
    return p;
}

PEHOST_WINAPI inline i32 HeapFree(u64 heap, u32, void* p) {
    if (heap != GetProcessHeap()) {
        set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    if (!p) return 1;
    if (!g_world.heap.free(p)) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return 1;
}

PEHOST_WINAPI inline void* HeapReAlloc(u64 heap, u32 flags, void* p, u64 bytes) {
    if (heap != GetProcessHeap()) {
        set_last_error(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    if (!p || !g_world.heap.owns(p)) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    u64 old = g_world.heap.usable(p);
    if ((flags & HEAP_REALLOC_IN_PLACE_ONLY) && bytes > old) {
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    void* q = g_world.heap.resize(p, bytes);
    if (!q) {
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    if (flags & HEAP_ZERO_MEMORY) {
        u8* b = static_cast<u8*>(q);
        u64 now = g_world.heap.usable(q);
        for (u64 i = old; i < now; ++i) b[i] = 0;
    }
    return q;
}

PEHOST_WINAPI inline u64 HeapSize(u64 heap, u32, const void* p) {
    if (heap != GetProcessHeap() || !g_world.heap.owns(p)) {
        set_last_error(heap != GetProcessHeap() ? ERROR_INVALID_HANDLE : ERROR_INVALID_PARAMETER);
        return ~0ull;
    }
    return g_world.heap.usable(p);
}

PEHOST_WINAPI inline u32 TlsAlloc() {
    for (u32 i = 0; i < TLS_SLOTS; ++i) {
        if (g_world.tls_used & (1ull << i)) continue;
        g_world.tls_used |= 1ull << i;
        detail::current_teb()->tls_slots[i] = 0;
        return i;
    }
    set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return TLS_OUT_OF_INDEXES;
}

PEHOST_WINAPI inline i32 TlsFree(u32 index) {
    if (index >= TLS_SLOTS || !(g_world.tls_used & (1ull << index))) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    g_world.tls_used &= ~(1ull << index);
    detail::current_teb()->tls_slots[index] = 0;
    return 1;
}

PEHOST_WINAPI inline void* TlsGetValue(u32 index) {
    if (index >= TLS_SLOTS) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    set_last_error(0);
    return reinterpret_cast<void*>(detail::current_teb()->tls_slots[index]);
}

PEHOST_WINAPI inline i32 TlsSetValue(u32 index, void* value) {
    if (index >= TLS_SLOTS) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    detail::current_teb()->tls_slots[index] = reinterpret_cast<u64>(value);
    return 1;
}

PEHOST_WINAPI inline void InitializeCriticalSection(CriticalSection* cs) {
    cs->debug_info = 0;
    cs->lock_count = -1;
    cs->recursion_count = 0;
    cs->owning_thread = 0;
    cs->lock_semaphore = 0;
    cs->spin_count = 0;
}

PEHOST_WINAPI inline void DeleteCriticalSection(CriticalSection* cs) {
    cs->lock_count = -1;
    cs->recursion_count = 0;
    cs->owning_thread = 0;
}

PEHOST_WINAPI inline void EnterCriticalSection(CriticalSection* cs) {
    ++cs->lock_count;
    ++cs->recursion_count;
    cs->owning_thread = detail::current_thread_id();
}

PEHOST_WINAPI inline i32 TryEnterCriticalSection(CriticalSection* cs) {
    if (cs->recursion_count != 0 && cs->owning_thread != detail::current_thread_id()) return 0;
    EnterCriticalSection(cs);
    return 1;
}

PEHOST_WINAPI inline void LeaveCriticalSection(CriticalSection* cs) {
    if (cs->recursion_count == 0) return;
    --cs->lock_count;
    if (--cs->recursion_count == 0) cs->owning_thread = 0;
}

PEHOST_WINAPI inline const char* GetCommandLineA() { return g_world.command_line; }

PEHOST_WINAPI inline const u16* GetCommandLineW() { return g_world.command_line_wide; }

PEHOST_WINAPI inline u64 GetModuleHandleA(const char* name) {
    if (name == nullptr) return reinterpret_cast<u64>(g_world.image);
    u64 token = detail::module_token(name);
    if (token == 0) set_last_error(ERROR_MOD_NOT_FOUND);
    return token;
}

PEHOST_WINAPI inline u64 GetModuleHandleW(const u16* name) {
    if (name == nullptr) return reinterpret_cast<u64>(g_world.image);
    u32 n = 0;
    while (name[n] && n < 63) ++n;
    if (name[n]) {
        set_last_error(ERROR_MOD_NOT_FOUND);
        return 0;
    }
    char narrow[64];
    i64 w = codec::utf16_to_utf8(name, n, reinterpret_cast<u8*>(narrow), sizeof(narrow) - 1, false);
    if (w < 0) {
        set_last_error(ERROR_MOD_NOT_FOUND);
        return 0;
    }
    narrow[w] = 0;
    return GetModuleHandleA(narrow);
}

PEHOST_WINAPI inline u64 LoadLibraryA(const char* name) {
    if (name == nullptr) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    u64 token = detail::module_token(name);
    if (token == 0) set_last_error(ERROR_MOD_NOT_FOUND);
    return token;
}

PEHOST_WINAPI inline u64 LoadLibraryW(const u16* name) {
    if (name == nullptr) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    u32 n = 0;
    while (name[n] && n < 127) ++n;
    if (name[n]) {
        set_last_error(ERROR_MOD_NOT_FOUND);
        return 0;
    }
    char narrow[128 * 3];
    i64 w = codec::utf16_to_utf8(name, n, reinterpret_cast<u8*>(narrow), sizeof(narrow) - 1, false);
    if (w < 0) {
        set_last_error(ERROR_MOD_NOT_FOUND);
        return 0;
    }
    narrow[w] = 0;
    return LoadLibraryA(narrow);
}

PEHOST_WINAPI inline i32 FreeLibrary(u64 module) {
    if (detail::module_name(module) == nullptr) {
        set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    return 1;
}

PEHOST_WINAPI inline void* GetProcAddress(u64 module, const char* name) {
    const char* dll = detail::module_name(module);
    if (dll == nullptr || reinterpret_cast<u64>(name) < 0x10000) {
        set_last_error(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }
    for (const ImportTable* t = g_world.imports; t; t = t->next) {
        for (u32 i = 0; i < t->count; ++i) {
            if (detail::same_dll(t->items[i].dll, dll) && detail::same_name(t->items[i].name, name)) return t->items[i].address;
        }
    }
    set_last_error(ERROR_PROC_NOT_FOUND);
    return nullptr;
}

PEHOST_WINAPI inline u32 GetModuleFileNameA(u64 module, char* buffer, u32 size) {
    if (module != 0 && module != reinterpret_cast<u64>(g_world.image)) {
        set_last_error(ERROR_MOD_NOT_FOUND);
        return 0;
    }
    if (size == 0) {
        set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    u32 length = detail::text_length(g_world.module_path);
    if (length >= size) {
        for (u32 i = 0; i + 1 < size; ++i) buffer[i] = g_world.module_path[i];
        buffer[size - 1] = 0;
        set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return size;
    }
    for (u32 i = 0; i <= length; ++i) buffer[i] = g_world.module_path[i];
    return length;
}

PEHOST_WINAPI inline u64 GetCurrentProcess() { return CURRENT_PROCESS; }

PEHOST_WINAPI inline u64 GetCurrentThread() { return CURRENT_THREAD; }

PEHOST_WINAPI inline u32 GetCurrentProcessId() { return static_cast<u32>(detail::current_teb()->client_process); }

PEHOST_WINAPI inline u32 GetCurrentThreadId() { return static_cast<u32>(detail::current_thread_id()); }

PEHOST_WINAPI inline void* SetUnhandledExceptionFilter(void* filter) {
    void* old = g_world.unhandled_filter;
    g_world.unhandled_filter = filter;
    return old;
}

PEHOST_WINAPI inline void Sleep(u32 milliseconds) {
    if (g_runtime->sleep_ms) g_runtime->sleep_ms(milliseconds);
}

PEHOST_WINAPI inline u64 VirtualQuery(const void* address, MemoryBasicInformation* info, u64 length) {
    u64 at = reinterpret_cast<u64>(address);
    u64 base = reinterpret_cast<u64>(g_world.image);
    if (length < sizeof(MemoryBasicInformation) || info == nullptr) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (g_world.image == nullptr || at < base || at - base >= static_cast<u64>(g_world.image_pages) * PAGE) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    u32 first = static_cast<u32>((at - base) / PAGE);
    u32 last = first;
    while (last + 1 < g_world.image_pages && g_world.page_prot[last + 1] == g_world.page_prot[first]) ++last;
    u32 protect = detail::windows_protect(g_world.page_prot[first]);
    info->base_address = base + static_cast<u64>(first) * PAGE;
    info->allocation_base = base;
    info->allocation_protect = protect;
    info->partition_id = 0;
    info->pad = 0;
    info->region_size = static_cast<u64>(last - first + 1) * PAGE;
    info->state = 0x1000;
    info->protect = protect;
    info->type = 0x1000000;
    info->pad2 = 0;
    return sizeof(MemoryBasicInformation);
}

PEHOST_WINAPI inline i32 VirtualProtect(void* address, u64 size, u32 protect, u32* old) {
    u64 at = reinterpret_cast<u64>(address);
    u64 base = reinterpret_cast<u64>(g_world.image);
    if (old == nullptr) {
        set_last_error(998);
        return 0;
    }
    u64 total = static_cast<u64>(g_world.image_pages) * PAGE;
    if (g_world.image == nullptr || size == 0 || at < base || at - base >= total || size > total - (at - base)) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    u32 native;
    if (!detail::native_protect(protect, &native)) {
        set_last_error(protect == 0x40 || protect == 0x80 ? ERROR_NOT_SUPPORTED : ERROR_INVALID_PARAMETER);
        return 0;
    }
    u32 first = static_cast<u32>((at - base) / PAGE);
    u32 last = static_cast<u32>((at - base + size - 1) / PAGE);
    u32 before = detail::windows_protect(g_world.page_prot[first]);
    if (!g_runtime->protect(g_world.image + static_cast<u64>(first) * PAGE, static_cast<u64>(last - first + 1) * PAGE, native)) {
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    for (u32 i = first; i <= last; ++i) g_world.page_prot[i] = static_cast<u8>(native);
    *old = before;
    return 1;
}

PEHOST_WINAPI inline i32 MultiByteToWideChar(u32 cp, u32 flags, const char* src, i32 length, u16* dst, i32 capacity) {
    if (!detail::code_page_ok(cp)) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if ((flags & ~(MB_ERR_INVALID_CHARS | MB_PRECOMPOSED)) != 0) {
        set_last_error(ERROR_INVALID_FLAGS);
        return 0;
    }
    if (src == nullptr || length == 0 || length < -1 || capacity < 0 || (capacity > 0 && dst == nullptr)) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    u64 n = length == -1 ? static_cast<u64>(detail::text_length(src)) + 1 : static_cast<u64>(length);
    i64 r = codec::utf8_to_utf16(reinterpret_cast<const u8*>(src), n, capacity == 0 ? nullptr : dst, static_cast<u64>(capacity), (flags & MB_ERR_INVALID_CHARS) != 0);
    if (r == codec::INVALID) {
        set_last_error(ERROR_NO_UNICODE_TRANSLATION);
        return 0;
    }
    if (r == codec::TOO_SMALL) {
        set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    return static_cast<i32>(r);
}

PEHOST_WINAPI inline i32 WideCharToMultiByte(u32 cp, u32 flags, const u16* src, i32 length, char* dst, i32 capacity, const char* default_char, i32* used_default) {
    if (!detail::code_page_ok(cp)) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (src == nullptr || length == 0 || length < -1 || capacity < 0 || (capacity > 0 && dst == nullptr)) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (cp == CODEPAGE_UTF8 && (default_char != nullptr || used_default != nullptr)) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (used_default) *used_default = 0;
    u64 n = static_cast<u64>(length);
    if (length == -1) {
        n = 0;
        while (src[n]) ++n;
        ++n;
    }
    i64 r = codec::utf16_to_utf8(src, n, capacity == 0 ? nullptr : reinterpret_cast<u8*>(dst), static_cast<u64>(capacity), (flags & WC_ERR_INVALID_CHARS) != 0);
    if (r == codec::INVALID) {
        set_last_error(ERROR_NO_UNICODE_TRANSLATION);
        return 0;
    }
    if (r == codec::TOO_SMALL) {
        set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    return static_cast<i32>(r);
}

PEHOST_WINAPI inline i32 IsDBCSLeadByteEx(u32, u8) { return 0; }

PEHOST_WINAPI inline u32 GetACP() { return CODEPAGE_UTF8; }

}

namespace pehost {

inline const ImportTable* kernel32_table() {
    static const Import items[] = {
        { "kernel32.dll", "ExitProcess", reinterpret_cast<void*>(&win32::ExitProcess) },
        { "kernel32.dll", "TerminateProcess", reinterpret_cast<void*>(&win32::TerminateProcess) },
        { "kernel32.dll", "GetStdHandle", reinterpret_cast<void*>(&win32::GetStdHandle) },
        { "kernel32.dll", "WriteFile", reinterpret_cast<void*>(&win32::WriteFile) },
        { "kernel32.dll", "GetLastError", reinterpret_cast<void*>(&win32::GetLastError) },
        { "kernel32.dll", "SetLastError", reinterpret_cast<void*>(&win32::SetLastError) },
        { "kernel32.dll", "GetProcessHeap", reinterpret_cast<void*>(&win32::GetProcessHeap) },
        { "kernel32.dll", "HeapAlloc", reinterpret_cast<void*>(&win32::HeapAlloc) },
        { "kernel32.dll", "HeapFree", reinterpret_cast<void*>(&win32::HeapFree) },
        { "kernel32.dll", "HeapReAlloc", reinterpret_cast<void*>(&win32::HeapReAlloc) },
        { "kernel32.dll", "HeapSize", reinterpret_cast<void*>(&win32::HeapSize) },
        { "kernel32.dll", "TlsAlloc", reinterpret_cast<void*>(&win32::TlsAlloc) },
        { "kernel32.dll", "TlsFree", reinterpret_cast<void*>(&win32::TlsFree) },
        { "kernel32.dll", "TlsGetValue", reinterpret_cast<void*>(&win32::TlsGetValue) },
        { "kernel32.dll", "TlsSetValue", reinterpret_cast<void*>(&win32::TlsSetValue) },
        { "kernel32.dll", "InitializeCriticalSection", reinterpret_cast<void*>(&win32::InitializeCriticalSection) },
        { "kernel32.dll", "DeleteCriticalSection", reinterpret_cast<void*>(&win32::DeleteCriticalSection) },
        { "kernel32.dll", "EnterCriticalSection", reinterpret_cast<void*>(&win32::EnterCriticalSection) },
        { "kernel32.dll", "TryEnterCriticalSection", reinterpret_cast<void*>(&win32::TryEnterCriticalSection) },
        { "kernel32.dll", "LeaveCriticalSection", reinterpret_cast<void*>(&win32::LeaveCriticalSection) },
        { "kernel32.dll", "GetCommandLineA", reinterpret_cast<void*>(&win32::GetCommandLineA) },
        { "kernel32.dll", "GetCommandLineW", reinterpret_cast<void*>(&win32::GetCommandLineW) },
        { "kernel32.dll", "GetModuleHandleA", reinterpret_cast<void*>(&win32::GetModuleHandleA) },
        { "kernel32.dll", "GetModuleHandleW", reinterpret_cast<void*>(&win32::GetModuleHandleW) },
        { "kernel32.dll", "GetProcAddress", reinterpret_cast<void*>(&win32::GetProcAddress) },
        { "kernel32.dll", "LoadLibraryA", reinterpret_cast<void*>(&win32::LoadLibraryA) },
        { "kernel32.dll", "LoadLibraryW", reinterpret_cast<void*>(&win32::LoadLibraryW) },
        { "kernel32.dll", "FreeLibrary", reinterpret_cast<void*>(&win32::FreeLibrary) },
        { "kernel32.dll", "GetModuleFileNameA", reinterpret_cast<void*>(&win32::GetModuleFileNameA) },
        { "kernel32.dll", "GetCurrentProcess", reinterpret_cast<void*>(&win32::GetCurrentProcess) },
        { "kernel32.dll", "GetCurrentThread", reinterpret_cast<void*>(&win32::GetCurrentThread) },
        { "kernel32.dll", "GetCurrentProcessId", reinterpret_cast<void*>(&win32::GetCurrentProcessId) },
        { "kernel32.dll", "GetCurrentThreadId", reinterpret_cast<void*>(&win32::GetCurrentThreadId) },
        { "kernel32.dll", "SetUnhandledExceptionFilter", reinterpret_cast<void*>(&win32::SetUnhandledExceptionFilter) },
        { "kernel32.dll", "Sleep", reinterpret_cast<void*>(&win32::Sleep) },
        { "kernel32.dll", "VirtualQuery", reinterpret_cast<void*>(&win32::VirtualQuery) },
        { "kernel32.dll", "VirtualProtect", reinterpret_cast<void*>(&win32::VirtualProtect) },
        { "kernel32.dll", "MultiByteToWideChar", reinterpret_cast<void*>(&win32::MultiByteToWideChar) },
        { "kernel32.dll", "WideCharToMultiByte", reinterpret_cast<void*>(&win32::WideCharToMultiByte) },
        { "kernel32.dll", "IsDBCSLeadByteEx", reinterpret_cast<void*>(&win32::IsDBCSLeadByteEx) },
        { "kernel32.dll", "GetACP", reinterpret_cast<void*>(&win32::GetACP) },
    };
    static const ImportTable table{ items, sizeof(items) / sizeof(items[0]) };
    return &table;
}

}
