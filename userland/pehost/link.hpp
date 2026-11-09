#pragma once
#include <cosmos/pe.hpp>
#include <cosmos/pe_modules.hpp>

#define PEHOST_WINAPI __attribute__((ms_abi))

namespace pehost {

struct Runtime {
    void (*exit_process)(u32 code) = nullptr;
    i64 (*write_console)(u32 stream, const void* data, u32 length) = nullptr;
    u8* (*reserve)(u64 bytes) = nullptr;
    bool (*protect)(u8* at, u64 bytes, u32 prot) = nullptr;
    void (*sleep_ms)(u32 milliseconds) = nullptr;
    u8* (*read_file)(const char* name, u64* size) = nullptr;
    void (*unmap)(u8* at, u64 bytes) = nullptr;
};

inline Runtime* g_runtime = nullptr;

struct Import {
    const char* dll;
    const char* name;
    void* address;
};

struct ImportTable {
    const Import* items;
    u32 count;
    const ImportTable* next = nullptr;
};

namespace detail {

inline char fold(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

inline bool same_dll(const char* a, const char* b) {
    while (*a && fold(*a) == fold(*b)) {
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

inline bool same_name(const char* a, const char* b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

inline bool lookup_builtin(const ImportTable* table, const char* home, const char* name, u64* address) {
    for (; table; table = table->next) {
        for (u32 i = 0; i < table->count; ++i) {
            const Import& e = table->items[i];
            if (same_dll(e.dll, home) && same_name(e.name, name)) {
                *address = reinterpret_cast<u64>(e.address);
                return true;
            }
        }
    }
    return false;
}

}

}
