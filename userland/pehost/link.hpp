#pragma once
#include <cosmos/pe.hpp>
#include <cosmos/pe_modules.hpp>

#define PEHOST_WINAPI __attribute__((ms_abi))

namespace pehost {

struct Clock {
    u64 counter;
    u64 frequency;
    u64 wall_ms;
    bool wall_valid;
};

struct FsInfo {
    u64 size;
    u64 mtime_ms;
    u32 kind;
    u32 reserved;
};

constexpr u32 FS_FILE = 0;
constexpr u32 FS_DIRECTORY = 1;
constexpr u32 FS_WANT_READ = 1;
constexpr u32 FS_WANT_WRITE = 2;
constexpr u32 FS_WANT_LIST = 4;
constexpr i64 FS_FAILED = -1;
constexpr i64 FS_DENIED = -7;
constexpr i64 FS_EXISTS = -12;
constexpr i64 FS_NOT_FOUND = -15;
constexpr i64 FS_WRONG_TYPE = -16;
constexpr i64 FS_NOT_EMPTY = -17;
constexpr i64 FS_BUSY = -18;
constexpr i64 FS_NO_SPACE = -19;

struct Runtime {
    void (*exit_process)(u32 code) = nullptr;
    i64 (*write_console)(u32 stream, const void* data, u32 length) = nullptr;
    u8* (*reserve)(u64 bytes) = nullptr;
    bool (*protect)(u8* at, u64 bytes, u32 prot) = nullptr;
    void (*sleep_ms)(u32 milliseconds) = nullptr;
    u8* (*read_file)(const char* name, u64* size) = nullptr;
    void (*unmap)(u8* at, u64 bytes) = nullptr;
    bool (*clock)(Clock* out) = nullptr;
    i64 (*file_open)(const char* name, u64* size) = nullptr;
    i64 (*file_read)(i64 handle, u64 offset, void* buffer, u64 length) = nullptr;
    void (*file_close)(i64 handle) = nullptr;
    i32 (*dir_entry)(u64 index, char* name, u32 capacity, u64* size, u64* mtime_ms, bool* directory) = nullptr;
    i64 (*read_stdin)(void* buffer, u32 length) = nullptr;
    i32 (*futex_wait)(volatile u32* word, u32 expected, u64 timeout_ms) = nullptr;
    void (*futex_wake)(volatile u32* word) = nullptr;
    bool (*spawn)(void (*entry)(u64), u64 argument, u8* stack_top) = nullptr;
    bool (*set_teb)(void* teb) = nullptr;
    i64 (*fs_open)(const char* path, u32 want, FsInfo* info) = nullptr;
    i64 (*fs_create)(const char* path, u32 directory, u32 want, FsInfo* info) = nullptr;
    i64 (*fs_read)(i64 handle, u64 offset, void* buffer, u64 length) = nullptr;
    i64 (*fs_write)(i64 handle, u64 offset, const void* buffer, u64 length) = nullptr;
    i64 (*fs_resize)(i64 handle, u64 size) = nullptr;
    void (*fs_close)(i64 handle) = nullptr;
    i64 (*fs_stat)(i64 handle, FsInfo* info) = nullptr;
    i64 (*fs_remove)(const char* path, u32 directory) = nullptr;
    i64 (*fs_rename)(const char* from, const char* to, u32 replace) = nullptr;
    i32 (*fs_entry)(i64 handle, u64 index, char* name, u32 capacity, FsInfo* info) = nullptr;
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
