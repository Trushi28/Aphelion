#pragma once
#include "host.hpp"

namespace pehost {

struct Runtime {
    void (*exit_process)(u32 code);
    i64 (*write_console)(u32 stream, const void* data, u32 length);
};

inline Runtime* g_runtime = nullptr;

constexpr u32 ERROR_INVALID_HANDLE = 6;
constexpr u32 ERROR_INVALID_PARAMETER = 87;
constexpr u32 STD_INPUT = 0xFFFFFFF6u;
constexpr u32 STD_OUTPUT = 0xFFFFFFF5u;
constexpr u32 STD_ERROR = 0xFFFFFFF4u;
constexpr u64 HANDLE_STDIN = 4;
constexpr u64 HANDLE_STDOUT = 8;
constexpr u64 HANDLE_STDERR = 12;
constexpr u64 INVALID_HANDLE = ~0ull;

inline void set_last_error(u32 code) { asm volatile("movl %0, %%gs:0x68" : : "r"(code) : "memory"); }

inline u32 last_error() {
    u32 code;
    asm volatile("movl %%gs:0x68, %0" : "=r"(code));
    return code;
}

}

#define PEHOST_WINAPI __attribute__((ms_abi))

namespace pehost::win32 {

PEHOST_WINAPI inline void ExitProcess(u32 code) {
    g_runtime->exit_process(code);
    for (;;) asm volatile("ud2");
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

}

namespace pehost {

inline const Import* kernel32_imports(u32* count) {
    static const Import table[] = {
        { "kernel32.dll", "ExitProcess", reinterpret_cast<void*>(&win32::ExitProcess) },
        { "kernel32.dll", "GetStdHandle", reinterpret_cast<void*>(&win32::GetStdHandle) },
        { "kernel32.dll", "WriteFile", reinterpret_cast<void*>(&win32::WriteFile) },
        { "kernel32.dll", "GetLastError", reinterpret_cast<void*>(&win32::GetLastError) },
        { "kernel32.dll", "SetLastError", reinterpret_cast<void*>(&win32::SetLastError) },
    };
    *count = sizeof(table) / sizeof(table[0]);
    return table;
}

}
