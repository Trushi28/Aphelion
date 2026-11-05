#pragma once
#include <cosmos/types.hpp>

namespace userabi {

constexpr u64 SYS_EXIT = 0;
constexpr u64 SYS_YIELD = 1;
constexpr u64 SYS_GETTID = 2;
constexpr u64 SYS_DEBUG_WRITE = 3;
constexpr u64 SYS_SET_GS_BASE = 4;
constexpr u64 SYS_GET_GS_BASE = 5;
constexpr u64 SYS_SET_FS_BASE = 6;
constexpr u64 SYS_GET_FS_BASE = 7;
constexpr u64 SYS_THREAD_CREATE = 8;
constexpr u64 SYS_THREAD_EXIT = 9;
constexpr u64 SYS_THREAD_JOIN = 10;
constexpr u64 SYS_HANDLE_CLOSE = 11;
constexpr u64 SYS_HANDLE_INFO = 12;
constexpr u64 SYS_HANDLE_NARROW = 13;
constexpr u64 SYS_CHANNEL_CREATE = 14;
constexpr u64 SYS_CHANNEL_SEND = 15;
constexpr u64 SYS_CHANNEL_RECV = 16;
constexpr u64 SYS_HANDLE_WAIT = 17;
constexpr u64 SYS_VMO_CREATE = 18;
constexpr u64 SYS_VM_MAP = 19;
constexpr u64 SYS_VM_UNMAP = 20;
constexpr u64 SYS_VM_PROTECT = 21;
constexpr u64 SYS_VMO_SIZE = 22;
constexpr u64 SYS_FILE_OPEN = 23;
constexpr u64 SYS_FILE_READ = 24;
constexpr u64 SYS_FILE_WRITE = 25;
constexpr u64 SYS_FILE_STAT = 26;
constexpr u64 SYS_FILE_CREATE = 27;
constexpr u64 SYS_DIR_ENTRY = 28;
constexpr u64 SYS_PIPE_CREATE = 29;
constexpr u64 SYS_PROCESS_CREATE = 30;
constexpr u64 SYS_PROCESS_STATUS = 31;
constexpr u64 SYS_PROCESS_KILL = 32;
constexpr u64 SYS_COUNT = 33;

constexpr i64 ERR_NOSYS = -1;
constexpr i64 ERR_FAULT = -2;
constexpr i64 ERR_INVAL = -3;
constexpr i64 ERR_LIMIT = -4;

constexpr u64 DEBUG_WRITE_MAX = 512;

constexpr i32 FAULT_STATUS_BASE = -256;
constexpr i32 FAULT_STATUS_FLOOR = FAULT_STATUS_BASE - 31;

constexpr i32 fault_status(u64 vector) { return FAULT_STATUS_BASE - static_cast<i32>(vector & 31); }
constexpr bool is_fault_status(i32 status) { return status <= FAULT_STATUS_BASE && status >= FAULT_STATUS_FLOOR; }
constexpr u64 fault_vector(i32 status) { return static_cast<u64>(FAULT_STATUS_BASE - status); }

constexpr i32 STATUS_KILLED = -512;

constexpr u32 MAX_LIVE_THREADS = 8;
constexpr u32 MAX_THREADS_TOTAL = 16;

constexpr u64 USER_LIMIT = 0x0000800000000000ull;

constexpr u64 CODE_BASE = 0x0000000000400000ull;
constexpr u64 DATA_BASE = 0x0000000000800000ull;
constexpr u64 STACK_TOP = 0x00007FFFFFF00000ull;
constexpr u32 STACK_PAGES = 8;
constexpr u32 MAX_CODE_PAGES = 64;
constexpr u32 MAX_DATA_PAGES = 64;

constexpr u64 SELECTOR_USER_DATA = 0x1B;
constexpr u64 SELECTOR_USER_CODE = 0x23;
constexpr u64 SYSCALL_FMASK = 0x47700;
constexpr u64 SYSRET_RFLAGS_KEEP = 0xCD5;
constexpr u64 USER_RFLAGS_INITIAL = 0x202;

}
