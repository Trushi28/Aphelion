#pragma once
#include <cosmos/types.hpp>
#include <cosmos/userabi.hpp>

namespace sys {

inline i64 call(u64 nr, u64 a = 0, u64 b = 0, u64 c = 0, u64 d = 0, u64 e = 0, u64 f = 0) {
    i64 result;
    register u64 r10 asm("r10") = d;
    register u64 r8 asm("r8") = e;
    register u64 r9 asm("r9") = f;
    asm volatile("syscall" : "=a"(result) : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return result;
}

[[noreturn]] inline void exit(i64 status) {
    call(userabi::SYS_EXIT, static_cast<u64>(status));
    for (;;) asm volatile("ud2");
}

inline i64 yield() { return call(userabi::SYS_YIELD); }
inline i64 gettid() { return call(userabi::SYS_GETTID); }
inline i64 debug_write(const char* text, u64 length) { return call(userabi::SYS_DEBUG_WRITE, reinterpret_cast<u64>(text), length); }
inline i64 set_gs_base(u64 base) { return call(userabi::SYS_SET_GS_BASE, base); }
inline i64 set_fs_base(u64 base) { return call(userabi::SYS_SET_FS_BASE, base); }

inline i64 handle_close(u64 handle) { return call(userabi::SYS_HANDLE_CLOSE, handle); }
inline i64 vmo_create(u64 pages, u64* out) { return call(userabi::SYS_VMO_CREATE, pages, 0, reinterpret_cast<u64>(out)); }
inline i64 vm_map(u64 vmo, u64 address, u64 offset_pages, u64 pages, u64 protection) {
    return call(userabi::SYS_VM_MAP, vmo, address, offset_pages, pages, protection);
}
inline i64 vm_unmap(u64 address, u64 pages) { return call(userabi::SYS_VM_UNMAP, address, pages); }
inline i64 vm_protect(u64 address, u64 pages, u64 protection) { return call(userabi::SYS_VM_PROTECT, address, pages, protection); }

inline i64 file_open(u64 dir, const char* path, u64 length, u64 rights, u64* out) {
    return call(userabi::SYS_FILE_OPEN, dir, reinterpret_cast<u64>(path), length, rights, reinterpret_cast<u64>(out));
}
inline i64 pipe_create(u64 bytes, u64* pair) { return call(userabi::SYS_PIPE_CREATE, bytes, reinterpret_cast<u64>(pair)); }
inline i64 handle_wait(void* entries, u64 count, u64 timeout_ms) { return call(userabi::SYS_HANDLE_WAIT, reinterpret_cast<u64>(entries), count, timeout_ms); }
inline i64 file_create(u64 dir, const char* path, u64 length, u64 rights, u64 flags, u64* out) {
    return call(userabi::SYS_FILE_CREATE, dir, reinterpret_cast<u64>(path), length, rights, flags, reinterpret_cast<u64>(out));
}
inline i64 file_unlink(u64 dir, const char* path, u64 length, u64 flags) {
    return call(userabi::SYS_FILE_UNLINK, dir, reinterpret_cast<u64>(path), length, flags);
}
inline i64 file_rename(u64 dir, const char* from, u64 from_length, const char* to, u64 to_length, u64 flags) {
    return call(userabi::SYS_FILE_RENAME, dir, reinterpret_cast<u64>(from), from_length, reinterpret_cast<u64>(to), to_length, flags);
}
inline i64 file_resize(u64 file, u64 size) { return call(userabi::SYS_FILE_RESIZE, file, size); }
inline i64 file_stat(u64 file, void* out) { return call(userabi::SYS_FILE_STAT, file, reinterpret_cast<u64>(out)); }
inline i64 file_read(u64 file, u64 offset, void* buffer, u64 length) { return call(userabi::SYS_FILE_READ, file, offset, reinterpret_cast<u64>(buffer), length); }
inline i64 dir_entry(u64 dir, u64 index, void* out) { return call(userabi::SYS_DIR_ENTRY, dir, index, reinterpret_cast<u64>(out)); }
inline i64 channel_recv(u64 channel, void* bytes, u64 capacity, u64* handles, u64 handle_capacity, u64 flags) {
    return call(userabi::SYS_CHANNEL_RECV, channel, reinterpret_cast<u64>(bytes), capacity, reinterpret_cast<u64>(handles), handle_capacity, flags);
}
inline i64 futex_wait(volatile u32* address, u32 expected, u64 timeout_ms) {
    return call(userabi::SYS_FUTEX_WAIT, reinterpret_cast<u64>(address), expected, timeout_ms);
}
inline i64 futex_wake(volatile u32* address, u64 count) { return call(userabi::SYS_FUTEX_WAKE, reinterpret_cast<u64>(address), count); }
inline i64 thread_create(void (*entry)(u64), u64 stack_top, u64 argument) {
    return call(userabi::SYS_THREAD_CREATE, reinterpret_cast<u64>(entry), stack_top, argument);
}
[[noreturn]] inline void thread_exit(i64 status) {
    call(userabi::SYS_THREAD_EXIT, static_cast<u64>(status));
    for (;;) asm volatile("ud2");
}
inline i64 thread_join(u64 tid) { return call(userabi::SYS_THREAD_JOIN, tid); }
inline i64 clock(void* out) { return call(userabi::SYS_CLOCK, reinterpret_cast<u64>(out)); }
inline i64 file_write(u64 file, u64 offset, const void* buffer, u64 length) {
    return call(userabi::SYS_FILE_WRITE, file, offset, reinterpret_cast<u64>(buffer), length);
}

}

constexpr u64 length_of(const char* text) {
    u64 n = 0;
    while (text[n]) ++n;
    return n;
}

inline void log(const char* text) { sys::debug_write(text, length_of(text)); }

extern "C" i64 aphelion_main(u64 argument, u64 second);
