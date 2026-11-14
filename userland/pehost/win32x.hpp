#pragma once
#include "fsys.hpp"
#include "objects.hpp"
#include "win32.hpp"

namespace pehost {

constexpr u32 ERROR_INVALID_FUNCTION = 1;
constexpr u32 ERROR_FILE_NOT_FOUND = 2;
constexpr u32 ERROR_PATH_NOT_FOUND = 3;
constexpr u32 ERROR_ACCESS_DENIED = 5;
constexpr u32 ERROR_BROKEN_PIPE = 109;
constexpr u32 ERROR_NEGATIVE_SEEK = 131;
constexpr u32 ERROR_ENVVAR_NOT_FOUND = 203;
constexpr u32 ERROR_TIMEOUT = 1460;
constexpr u32 ERROR_GEN_FAILURE = 31;
constexpr u32 ERROR_OLD_WIN_VERSION = 1150;
constexpr u32 INVALID_FILE_ATTRIBUTES = 0xFFFFFFFFu;
constexpr u32 FLS_OUT_OF_INDEXES = 0xFFFFFFFFu;
constexpr u32 STATUS_NOT_SUPPORTED = 0xC00000BBu;
constexpr u32 MAX_FLS = 128;
constexpr u32 MAX_OPEN_FILES = 32;
constexpr u32 MAX_ENV = 32;
constexpr u32 MAX_ENV_NAME = 64;
constexpr u32 MAX_ENV_VALUE = 256;
constexpr u32 MAX_NAME = 200;
constexpr u64 FILE_HANDLE_BASE = 0x100;
constexpr u64 FILETIME_UNIX_EPOCH_MS = 11644473600000ull;

struct FlsSlot {
    bool used;
    void (PEHOST_WINAPI* callback)(void*);
};

struct OpenFile {
    bool used;
    i64 platform;
    u64 size;
    u64 position;
    bool work;
    bool writable;
    bool directory;
    bool delete_on_close;
};

struct EnvVar {
    bool used;
    char name[MAX_ENV_NAME];
    char value[MAX_ENV_VALUE];
};

struct Extra {
    FlsSlot fls[MAX_FLS];
    u64 fallback_fls[MAX_FLS];
    OpenFile files[MAX_OPEN_FILES];
    char delete_paths[MAX_OPEN_FILES][256];
    EnvVar env[MAX_ENV];
};

inline Extra g_extra;

static_assert((TLS_SLOTS + MAX_FLS) * 8 <= PAGE);

namespace detail {

inline u64 scale_ms(u64 counter, u64 frequency) {
    if (frequency == 0) return 0;
    return counter / frequency * 1000 + counter % frequency * 1000 / frequency;
}

inline u64 filetime_of(u64 ms) { return (ms + FILETIME_UNIX_EPOCH_MS) * 10000; }

inline void put_filetime(void* at, u64 value) {
    u32* two = static_cast<u32*>(at);
    two[0] = static_cast<u32>(value);
    two[1] = static_cast<u32>(value >> 32);
}

inline bool read_clock(Clock* out) {
    if (!g_runtime->clock) return false;
    return g_runtime->clock(out) && out->frequency != 0;
}

inline bool narrow_name(const u16* wide, char* out) {
    if (wide == nullptr) return false;
    u32 n = 0;
    while (wide[n] && n < MAX_NAME) ++n;
    if (wide[n]) return false;
    i64 w = codec::utf16_to_utf8(wide, n, reinterpret_cast<u8*>(out), MAX_NAME * 3, true);
    if (w < 0 || w > static_cast<i64>(MAX_NAME)) return false;
    out[w] = 0;
    return true;
}

inline bool plain_file_name(const char* name) {
    if (name == nullptr || name[0] == 0) return false;
    u32 n = 0;
    for (; name[n]; ++n) {
        if (n >= MAX_NAME || name[n] == '\\' || name[n] == '/' || name[n] == ':') return false;
    }
    return !(n == 1 && name[0] == '.') && !(n == 2 && name[0] == '.' && name[1] == '.');
}

inline OpenFile* file_of(u64 handle) {
    if (handle < FILE_HANDLE_BASE || (handle - FILE_HANDLE_BASE) % 4 != 0) return nullptr;
    u64 index = (handle - FILE_HANDLE_BASE) / 4;
    if (index >= MAX_OPEN_FILES || !g_extra.files[index].used) return nullptr;
    return &g_extra.files[index];
}

inline bool is_std_handle(u64 h) { return h == HANDLE_STDIN || h == HANDLE_STDOUT || h == HANDLE_STDERR; }

inline bool equal_text(const char* a, const char* b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

inline bool equal_folded(const char* a, const char* b) {
    while (*a && fold(*a) == fold(*b)) {
        ++a;
        ++b;
    }
    return fold(*a) == fold(*b);
}

inline u16 upper16(u16 c) { return c >= 'a' && c <= 'z' ? static_cast<u16>(c - 32) : c; }
inline u16 lower16(u16 c) { return c >= 'A' && c <= 'Z' ? static_cast<u16>(c + 32) : c; }

inline i64 open_for_read(const char* name, u64* size) {
    if (!g_runtime->file_open) return -1;
    return g_runtime->file_open(name, size);
}

inline i32 open_failure(const char* name) {
    set_last_error(plain_file_name(name) ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND);
    return 0;
}

}

namespace win32 {

PEHOST_WINAPI inline i32 QueryPerformanceFrequency(i64* out) {
    Clock c;
    if (out == nullptr || !detail::read_clock(&c)) {
        set_last_error(out == nullptr ? ERROR_INVALID_PARAMETER : ERROR_NOT_SUPPORTED);
        return 0;
    }
    *out = static_cast<i64>(c.frequency);
    return 1;
}

PEHOST_WINAPI inline i32 QueryPerformanceCounter(i64* out) {
    Clock c;
    if (out == nullptr || !detail::read_clock(&c)) {
        set_last_error(out == nullptr ? ERROR_INVALID_PARAMETER : ERROR_NOT_SUPPORTED);
        return 0;
    }
    *out = static_cast<i64>(c.counter);
    return 1;
}

PEHOST_WINAPI inline void GetSystemTimeAsFileTime(u32* out) {
    Clock c;
    u64 ms = 0;
    if (detail::read_clock(&c)) ms = c.wall_valid ? c.wall_ms : detail::scale_ms(c.counter, c.frequency);
    detail::put_filetime(out, (ms + (c.wall_valid ? FILETIME_UNIX_EPOCH_MS : 0)) * 10000);
}

PEHOST_WINAPI inline u64 GetTickCount64() {
    Clock c;
    return detail::read_clock(&c) ? detail::scale_ms(c.counter, c.frequency) : 0;
}

PEHOST_WINAPI inline u32 GetTickCount() { return static_cast<u32>(GetTickCount64()); }

PEHOST_WINAPI inline i32 IsDebuggerPresent() { return 0; }

PEHOST_WINAPI inline i32 IsProcessorFeaturePresent(u32 feature) {
    switch (feature) {
        case 6:
        case 10:
        case 13:
        case 14:
        case 17: return 1;
        default: return 0;
    }
}

PEHOST_WINAPI inline u32 GetActiveProcessorCount(u16) { return 1; }

PEHOST_WINAPI inline void GetStartupInfoW(u8* info) {
    for (u32 i = 0; i < 104; ++i) info[i] = 0;
    *reinterpret_cast<u32*>(info) = 104;
}

PEHOST_WINAPI inline void GetStartupInfoA(u8* info) { GetStartupInfoW(info); }

PEHOST_WINAPI inline u64 EncodePointer(u64 p) { return p; }

PEHOST_WINAPI inline u64 DecodePointer(u64 p) { return p; }

PEHOST_WINAPI inline void InitializeSListHead(u64* head) { head[0] = head[1] = 0; }

PEHOST_WINAPI inline u64 InterlockedPushEntrySList(u64* head, u64* entry) {
    u64 old = head[0];
    entry[0] = old;
    head[0] = reinterpret_cast<u64>(entry);
    head[1] = (head[1] & ~0xFFFFull) | ((head[1] & 0xFFFF) + 1);
    return old;
}

PEHOST_WINAPI inline u64 InterlockedPopEntrySList(u64* head) {
    u64 top = head[0];
    if (top == 0) return 0;
    head[0] = *reinterpret_cast<u64*>(top);
    head[1] = (head[1] & ~0xFFFFull) | ((head[1] & 0xFFFF) - 1);
    return top;
}

PEHOST_WINAPI inline u64 InterlockedFlushSList(u64* head) {
    u64 top = head[0];
    head[0] = 0;
    head[1] &= ~0xFFFFull;
    return top;
}

PEHOST_WINAPI inline u16 QueryDepthSList(const u64* head) { return static_cast<u16>(head[1] & 0xFFFF); }

PEHOST_WINAPI inline i32 InitializeCriticalSectionEx(CriticalSection* cs, u32, u32) {
    InitializeCriticalSection(cs);
    return 1;
}

PEHOST_WINAPI inline i32 InitializeCriticalSectionAndSpinCount(CriticalSection* cs, u32) {
    InitializeCriticalSection(cs);
    return 1;
}

PEHOST_WINAPI inline u32 SetCriticalSectionSpinCount(CriticalSection*, u32) { return 0; }

namespace syncd {

inline u64* fls_values_of(const Teb* t) { return t->fls_data ? reinterpret_cast<u64*>(t->fls_data) : g_extra.fallback_fls; }

inline u32 fls_arrays(u64** out) {
    u32 n = 0;
    u64* mine = fls_values_of(pehost::detail::current_teb());
    out[n++] = mine;
    for (u32 i = 0; i < sync::MAX_SLOTS; ++i) {
        const sync::Slot& s = sync::g_sync.slots[i];
        if (s.used && s.fls && s.fls != mine) out[n++] = s.fls;
    }
    return n;
}

}

inline void fls_thread_exit(u64* values) {
    for (u32 i = 0; i < MAX_FLS; ++i) {
        void (PEHOST_WINAPI* callback)(void*) = nullptr;
        u64 value = 0;
        {
            sync::Host guard;
            if (g_extra.fls[i].used && g_extra.fls[i].callback && values[i]) {
                callback = g_extra.fls[i].callback;
                value = values[i];
            }
            values[i] = 0;
        }
        if (callback) callback(reinterpret_cast<void*>(value));
    }
}

PEHOST_WINAPI inline u32 FlsAlloc(void (PEHOST_WINAPI* callback)(void*)) {
    sync::Host guard;
    for (u32 i = 0; i < MAX_FLS; ++i) {
        if (!g_extra.fls[i].used) {
            g_extra.fls[i] = FlsSlot{ true, callback };
            u64* arrays[sync::MAX_SLOTS];
            u32 n = syncd::fls_arrays(arrays);
            for (u32 k = 0; k < n; ++k) arrays[k][i] = 0;
            return i;
        }
    }
    set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return FLS_OUT_OF_INDEXES;
}

PEHOST_WINAPI inline i32 FlsFree(u32 index) {
    void (PEHOST_WINAPI* callback)(void*) = nullptr;
    u64 pending[sync::MAX_SLOTS];
    u32 pending_count = 0;
    {
        sync::Host guard;
        if (index >= MAX_FLS || !g_extra.fls[index].used) {
            set_last_error(ERROR_INVALID_PARAMETER);
            return 0;
        }
        callback = g_extra.fls[index].callback;
        g_extra.fls[index] = FlsSlot{ false, nullptr };
        u64* arrays[sync::MAX_SLOTS];
        u32 n = syncd::fls_arrays(arrays);
        for (u32 k = 0; k < n; ++k) {
            if (arrays[k][index]) pending[pending_count++] = arrays[k][index];
            arrays[k][index] = 0;
        }
    }
    if (callback) {
        for (u32 k = 0; k < pending_count; ++k) callback(reinterpret_cast<void*>(pending[k]));
    }
    return 1;
}

PEHOST_WINAPI inline void* FlsGetValue(u32 index) {
    sync::Host guard;
    if (index >= MAX_FLS || !g_extra.fls[index].used) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    set_last_error(0);
    return reinterpret_cast<void*>(syncd::fls_values_of(detail::current_teb())[index]);
}

PEHOST_WINAPI inline i32 FlsSetValue(u32 index, void* value) {
    sync::Host guard;
    if (index >= MAX_FLS || !g_extra.fls[index].used) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    syncd::fls_values_of(detail::current_teb())[index] = reinterpret_cast<u64>(value);
    return 1;
}

namespace syncd {

constexpr u32 SRW_EXCLUSIVE = 1;
constexpr u32 SRW_SHARED_UNIT = 2;

inline volatile u32* srw_state(u64* lock) { return reinterpret_cast<volatile u32*>(lock); }
inline volatile u32* srw_waiters(u64* lock) { return reinterpret_cast<volatile u32*>(lock) + 1; }

inline void srw_wait(u64* lock, u32 observed) {
    sync::add(srw_waiters(lock), 1);
    if (sync::load(srw_state(lock)) == observed) sync::block(srw_state(lock), observed, sync::FOREVER);
    sync::add(srw_waiters(lock), static_cast<u32>(-1));
}

inline void srw_released(u64* lock) {
    if (sync::load(srw_waiters(lock)) != 0) sync::wake(srw_state(lock));
}

inline bool timed_wait(volatile u32* word, u32 expected, u32 milliseconds) {
    if (milliseconds == 0xFFFFFFFFu) {
        sync::block(word, expected, sync::FOREVER);
        return true;
    }
    if (!sync::available() || milliseconds == 0) {
        if (g_runtime->sleep_ms) g_runtime->sleep_ms(milliseconds);
        return false;
    }
    return sync::block(word, expected, milliseconds) == 0;
}

}

PEHOST_WINAPI inline void InitializeSRWLock(u64* lock) { *lock = 0; }

PEHOST_WINAPI inline u8 TryAcquireSRWLockExclusive(u64* lock) { return sync::cas(syncd::srw_state(lock), 0, syncd::SRW_EXCLUSIVE) ? 1 : 0; }

PEHOST_WINAPI inline void AcquireSRWLockExclusive(u64* lock) {
    for (;;) {
        if (sync::cas(syncd::srw_state(lock), 0, syncd::SRW_EXCLUSIVE)) return;
        u32 now = sync::load(syncd::srw_state(lock));
        if (now != 0) syncd::srw_wait(lock, now);
    }
}

PEHOST_WINAPI inline void ReleaseSRWLockExclusive(u64* lock) {
    sync::store(syncd::srw_state(lock), 0);
    syncd::srw_released(lock);
}

PEHOST_WINAPI inline u8 TryAcquireSRWLockShared(u64* lock) {
    for (;;) {
        u32 now = sync::load(syncd::srw_state(lock));
        if (now & syncd::SRW_EXCLUSIVE) return 0;
        if (sync::cas(syncd::srw_state(lock), now, now + syncd::SRW_SHARED_UNIT)) return 1;
    }
}

PEHOST_WINAPI inline void AcquireSRWLockShared(u64* lock) {
    for (;;) {
        u32 now = sync::load(syncd::srw_state(lock));
        if (!(now & syncd::SRW_EXCLUSIVE)) {
            if (sync::cas(syncd::srw_state(lock), now, now + syncd::SRW_SHARED_UNIT)) return;
            continue;
        }
        syncd::srw_wait(lock, now);
    }
}

PEHOST_WINAPI inline void ReleaseSRWLockShared(u64* lock) {
    for (;;) {
        u32 now = sync::load(syncd::srw_state(lock));
        if (now < syncd::SRW_SHARED_UNIT || (now & syncd::SRW_EXCLUSIVE)) return;
        if (sync::cas(syncd::srw_state(lock), now, now - syncd::SRW_SHARED_UNIT)) {
            if (now - syncd::SRW_SHARED_UNIT == 0) syncd::srw_released(lock);
            return;
        }
    }
}

PEHOST_WINAPI inline void InitializeConditionVariable(u64* variable) { *variable = 0; }

PEHOST_WINAPI inline void WakeConditionVariable(u64* variable) {
    volatile u32* sequence = reinterpret_cast<volatile u32*>(variable);
    volatile u32* waiters = sequence + 1;
    if (sync::load(waiters) == 0) return;
    sync::add(sequence, 1);
    sync::wake(sequence);
}

PEHOST_WINAPI inline void WakeAllConditionVariable(u64* variable) { WakeConditionVariable(variable); }

PEHOST_WINAPI inline i32 SleepConditionVariableSRW(u64* variable, u64* lock, u32 milliseconds, u32 flags) {
    volatile u32* sequence = reinterpret_cast<volatile u32*>(variable);
    volatile u32* waiters = sequence + 1;
    bool shared = (flags & 1) != 0;
    sync::add(waiters, 1);
    u32 seen = sync::load(sequence);
    if (shared) ReleaseSRWLockShared(lock);
    else ReleaseSRWLockExclusive(lock);
    bool woken = syncd::timed_wait(sequence, seen, milliseconds);
    sync::add(waiters, static_cast<u32>(-1));
    if (shared) AcquireSRWLockShared(lock);
    else AcquireSRWLockExclusive(lock);
    if (!woken) set_last_error(ERROR_TIMEOUT);
    return woken ? 1 : 0;
}

PEHOST_WINAPI inline i32 SleepConditionVariableCS(u64* variable, CriticalSection* cs, u32 milliseconds) {
    volatile u32* sequence = reinterpret_cast<volatile u32*>(variable);
    volatile u32* waiters = sequence + 1;
    sync::add(waiters, 1);
    u32 seen = sync::load(sequence);
    i32 depth = cs->recursion_count;
    for (i32 i = 0; i < depth; ++i) LeaveCriticalSection(cs);
    bool woken = syncd::timed_wait(sequence, seen, milliseconds);
    sync::add(waiters, static_cast<u32>(-1));
    for (i32 i = 0; i < depth; ++i) EnterCriticalSection(cs);
    if (!woken) set_last_error(ERROR_TIMEOUT);
    return woken ? 1 : 0;
}

PEHOST_WINAPI inline i32 InitOnceBeginInitialize(u64* once, u32 flags, i32* pending, void** context) {
    constexpr u32 ASYNC = 2, CHECK_ONLY = 1;
    volatile u32* word = reinterpret_cast<volatile u32*>(once);
    for (;;) {
        u32 now = sync::load(word);
        u32 state = now & 3;
        if (state == 2) {
            if (pending) *pending = 0;
            if (context) *context = reinterpret_cast<void*>(*once & ~3ull);
            return 1;
        }
        if (flags & CHECK_ONLY) {
            set_last_error(ERROR_GEN_FAILURE);
            return 0;
        }
        if (state == 0) {
            if (!sync::cas(word, now, now | 1)) continue;
            if (pending) *pending = 1;
            return 1;
        }
        if (flags & ASYNC) {
            if (pending) *pending = 1;
            return 1;
        }
        sync::block(word, now, sync::FOREVER);
    }
}

PEHOST_WINAPI inline i32 InitOnceComplete(u64* once, u32 flags, void* context) {
    constexpr u32 FAILED = 4;
    volatile u32* word = reinterpret_cast<volatile u32*>(once);
    u64 value = reinterpret_cast<u64>(context);
    if ((value & 3) != 0) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (flags & FAILED) {
        sync::store(word, 0);
        sync::wake(word);
        return 1;
    }
    __atomic_store_n(once, value | 2, __ATOMIC_SEQ_CST);
    sync::wake(word);
    return 1;
}

PEHOST_WINAPI inline i32 InitOnceExecuteOnce(u64* once, i32 (PEHOST_WINAPI* function)(u64*, void*, void**), void* parameter, void** context) {
    i32 pending = 0;
    void* stored = nullptr;
    if (!InitOnceBeginInitialize(once, 0, &pending, &stored)) return 0;
    if (!pending) {
        if (context) *context = stored;
        return 1;
    }
    void* made = nullptr;
    i32 ok = function(once, parameter, &made);
    if (!ok) {
        InitOnceComplete(once, 4, nullptr);
        return 0;
    }
    if (!InitOnceComplete(once, 0, made)) return 0;
    if (context) *context = made;
    return 1;
}

PEHOST_WINAPI inline void InitOnceInitialize(u64* once) { *once = 0; }

namespace syncd {

constexpr u32 ADDRESS_BUCKETS = 64;
inline volatile u32 g_address_buckets[ADDRESS_BUCKETS];

inline volatile u32* address_bucket(const void* address) {
    u64 a = reinterpret_cast<u64>(address) >> 2;
    return &g_address_buckets[(a ^ (a >> 6) ^ (a >> 12)) % ADDRESS_BUCKETS];
}

inline bool same_bytes(const volatile void* a, const void* b, u64 size) {
    const volatile u8* x = static_cast<const volatile u8*>(a);
    const u8* y = static_cast<const u8*>(b);
    for (u64 i = 0; i < size; ++i) {
        if (x[i] != y[i]) return false;
    }
    return true;
}

}

PEHOST_WINAPI inline i32 WaitOnAddress(volatile void* address, void* compare, u64 size, u32 milliseconds) {
    if (address == nullptr || compare == nullptr || (size != 1 && size != 2 && size != 4 && size != 8)) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    volatile u32* bucket = syncd::address_bucket(const_cast<const void*>(address));
    sync::Deadline deadline(milliseconds == 0xFFFFFFFFu ? sync::FOREVER : milliseconds);
    for (;;) {
        u32 seen = sync::load(bucket);
        if (!syncd::same_bytes(address, compare, size)) return 1;
        if (milliseconds == 0 || deadline.expired()) break;
        if (sync::block(bucket, seen, deadline.remaining()) == 1) break;
        if (sync::load(bucket) != seen) return 1;
    }
    set_last_error(ERROR_TIMEOUT);
    return 0;
}

PEHOST_WINAPI inline void WakeByAddressSingle(void* address) {
    volatile u32* bucket = syncd::address_bucket(address);
    sync::add(bucket, 1);
    sync::wake(bucket);
}

PEHOST_WINAPI inline void WakeByAddressAll(void* address) { WakeByAddressSingle(address); }

PEHOST_WINAPI inline void RtlCaptureContext(u8* context) {
    for (u32 i = 0; i < 1232; ++i) context[i] = 0;
    *reinterpret_cast<u32*>(context + 0x30) = 0x10000F;
}

PEHOST_WINAPI inline void* RtlLookupFunctionEntry(u64, u64* base, void*) {
    if (base) *base = 0;
    return nullptr;
}

PEHOST_WINAPI inline u64 RtlPcToFileHeader(u64 pc, u64* base) {
    u64 found = 0;
    u64 image = reinterpret_cast<u64>(g_world.image);
    if (g_world.image && pc >= image && pc - image < static_cast<u64>(g_world.image_pages) * PAGE) found = image;
    for (u32 i = 0; i < modules::MAX_MODULES && found == 0; ++i) {
        const modules::Module& m = modules::g_state.items[i];
        u64 b = reinterpret_cast<u64>(m.base);
        if (m.used && !m.binding && pc >= b && pc - b < m.bytes) found = b;
    }
    if (base) *base = found;
    return found;
}

PEHOST_WINAPI inline void RaiseException(u32 code, u32, u32, const u64*) { detail::stop("an exception was raised and nothing here can handle it", code); }

PEHOST_WINAPI inline i32 UnhandledExceptionFilter(void*) { return 0; }

PEHOST_WINAPI inline void RtlUnwind(void*, void*, void*, void*) { detail::stop("structured exception unwinding is not supported", STATUS_NOT_SUPPORTED); }

PEHOST_WINAPI inline void RtlUnwindEx(void*, void*, void*, void*, void*, void*) { detail::stop("structured exception unwinding is not supported", STATUS_NOT_SUPPORTED); }

PEHOST_WINAPI inline void* RtlVirtualUnwind(u32, u64, u64, void*, void*, void*, void*, void*) {
    detail::stop("structured exception unwinding is not supported", STATUS_NOT_SUPPORTED);
}

PEHOST_WINAPI inline u32 GetModuleFileNameW(u64 module, u16* buffer, u32 size) {
    sync::Loader guard;
    char narrow[MAX_MODULE_PATH];
    u32 n = GetModuleFileNameA(module, narrow, sizeof(narrow));
    if (n == 0) return 0;
    if (size == 0) {
        set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    u16 wide[MAX_MODULE_PATH];
    i64 w = codec::utf8_to_utf16(reinterpret_cast<const u8*>(narrow), n, wide, MAX_MODULE_PATH - 1, false);
    if (w < 0) w = 0;
    u32 count = static_cast<u32>(w);
    bool cut = count >= size;
    if (cut) count = size - 1;
    for (u32 i = 0; i < count; ++i) buffer[i] = wide[i];
    buffer[count] = 0;
    if (cut) {
        set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return size;
    }
    return count;
}

PEHOST_WINAPI inline u64 LoadLibraryExA(const char* name, u64 reserved, u32 flags) {
    sync::Loader guard;
    if (reserved != 0 || (flags & (0x1 | 0x2 | 0x10 | 0x20 | 0x40)) != 0) {
        set_last_error(reserved != 0 ? ERROR_INVALID_PARAMETER : ERROR_NOT_SUPPORTED);
        return 0;
    }
    return LoadLibraryA(name);
}

PEHOST_WINAPI inline u64 LoadLibraryExW(const u16* name, u64 reserved, u32 flags) {
    sync::Loader guard;
    if (reserved != 0 || (flags & (0x1 | 0x2 | 0x10 | 0x20 | 0x40)) != 0) {
        set_last_error(reserved != 0 ? ERROR_INVALID_PARAMETER : ERROR_NOT_SUPPORTED);
        return 0;
    }
    return LoadLibraryW(name);
}

PEHOST_WINAPI inline i32 GetModuleHandleExW(u32 flags, const void* what, u64* out) {
    sync::Loader guard;
    if (out == nullptr || (flags & ~7u) != 0 || (flags & 3) == 3) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    *out = 0;
    u64 handle = 0;
    if (flags & 4) {
        u64 at = reinterpret_cast<u64>(what);
        RtlPcToFileHeader(at, &handle);
        if (handle == 0) {
            set_last_error(ERROR_MOD_NOT_FOUND);
            return 0;
        }
    } else {
        handle = GetModuleHandleW(static_cast<const u16*>(what));
        if (handle == 0) return 0;
    }
    i32 found = modules::find_base(handle);
    if (found >= 0 && (flags & 3) == 0) ++modules::g_state.items[found].refs;
    *out = handle;
    return 1;
}

PEHOST_WINAPI inline u64 GetFileType(u64 handle) {
    if (detail::is_std_handle(handle)) return 3;
    if (detail::file_of(handle)) return 1;
    set_last_error(ERROR_INVALID_HANDLE);
    return 0;
}

PEHOST_WINAPI inline i32 GetConsoleMode(u64 handle, u32*) {
    set_last_error(ERROR_INVALID_HANDLE);
    (void)handle;
    return 0;
}

PEHOST_WINAPI inline i32 SetConsoleMode(u64, u32) {
    set_last_error(ERROR_INVALID_HANDLE);
    return 0;
}

PEHOST_WINAPI inline i32 GetConsoleScreenBufferInfo(u64, void*) {
    set_last_error(ERROR_INVALID_HANDLE);
    return 0;
}

PEHOST_WINAPI inline u32 GetConsoleOutputCP() { return CODEPAGE_UTF8; }

PEHOST_WINAPI inline u32 GetConsoleCP() { return CODEPAGE_UTF8; }

PEHOST_WINAPI inline u32 GetOEMCP() { return CODEPAGE_UTF8; }

PEHOST_WINAPI inline i32 SetConsoleCtrlHandler(void*, i32) {
    set_last_error(ERROR_NOT_SUPPORTED);
    return 0;
}

PEHOST_WINAPI inline i32 SetStdHandle(u32, u64) {
    set_last_error(ERROR_NOT_SUPPORTED);
    return 0;
}

PEHOST_WINAPI inline i32 WriteConsoleW(u64 handle, const u16* text, u32 count, u32* written, void* reserved) {
    if (written) *written = 0;
    if ((handle != HANDLE_STDOUT && handle != HANDLE_STDERR) || reserved != nullptr) {
        set_last_error(handle == HANDLE_STDOUT || handle == HANDLE_STDERR ? ERROR_INVALID_PARAMETER : ERROR_INVALID_HANDLE);
        return 0;
    }
    u8 chunk[512];
    u32 done = 0;
    while (done < count) {
        u32 take = count - done > 128 ? 128 : count - done;
        if (take > 1 && text[done + take - 1] >= 0xD800 && text[done + take - 1] < 0xDC00) --take;
        i64 w = codec::utf16_to_utf8(text + done, take, chunk, sizeof(chunk), false);
        if (w < 0) {
            set_last_error(ERROR_NO_UNICODE_TRANSLATION);
            return 0;
        }
        g_runtime->write_console(handle == HANDLE_STDERR ? 2 : 1, chunk, static_cast<u32>(w));
        done += take;
    }
    if (written) *written = count;
    return 1;
}

PEHOST_WINAPI inline i32 ReadConsoleW(u64, void*, u32, u32*, void*) {
    set_last_error(ERROR_INVALID_HANDLE);
    return 0;
}

PEHOST_WINAPI inline i32 FlushFileBuffers(u64 handle) {
    if (detail::is_std_handle(handle) || detail::file_of(handle)) return 1;
    set_last_error(ERROR_INVALID_HANDLE);
    return 0;
}

inline i32 write_file_handle(u64 handle, const void* data, u32 length, u32* written, void* overlapped) {
    if (overlapped != nullptr) {
        set_last_error(ERROR_NOT_SUPPORTED);
        return 0;
    }
    sync::Host guard;
    OpenFile* f = detail::file_of(handle);
    if (!f) {
        set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    if (!f->work || !f->writable || f->directory || !g_runtime->fs_write) {
        set_last_error(ERROR_ACCESS_DENIED);
        return 0;
    }
    if (length == 0) return 1;
    i64 n = fsys::write_at(f->platform, f->position, data, length);
    if (n < 0) {
        set_last_error(fsys::translate(n, ERROR_ACCESS_DENIED));
        return 0;
    }
    f->position += static_cast<u64>(n);
    if (f->position > f->size) f->size = f->position;
    if (written) *written = static_cast<u32>(n);
    if (static_cast<u64>(n) != length) {
        set_last_error(fsys::E_DISK_FULL);
        return 0;
    }
    return 1;
}

PEHOST_WINAPI inline i32 CloseHandle(u64 handle) {
    if (detail::is_std_handle(handle)) return 1;
    if (objects::is_object_handle(handle)) {
        if (objects::close(handle)) return 1;
        set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    sync::Host guard;
    OpenFile* f = detail::file_of(handle);
    if (!f) {
        set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    u32 index = static_cast<u32>(f - g_extra.files);
    if (f->work) {
        if (g_runtime->fs_close) g_runtime->fs_close(f->platform);
        if (f->delete_on_close && g_runtime->fs_remove) g_runtime->fs_remove(g_extra.delete_paths[index], 0);
    } else if (g_runtime->file_close) {
        g_runtime->file_close(f->platform);
    }
    *f = OpenFile{};
    g_extra.delete_paths[index][0] = 0;
    return 1;
}

inline u32 free_file_slot() {
    for (u32 i = 0; i < MAX_OPEN_FILES; ++i) {
        if (!g_extra.files[i].used) return i;
    }
    return MAX_OPEN_FILES;
}

PEHOST_WINAPI inline u64 CreateFileA(const char* name, u32 access, u32, void*, u32 disposition, u32 flags, u64) {
    sync::Host guard;
    if (name == nullptr) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE;
    }
    if (detail::equal_text(name, "CONOUT$")) return HANDLE_STDOUT;
    if (detail::equal_text(name, "CONIN$")) return HANDLE_STDIN;
    if (flags & 0x40000000u) {
        set_last_error(ERROR_NOT_SUPPORTED);
        return INVALID_HANDLE;
    }
    if (!fsys::enabled()) {
        bool writes = (access & (0x40000000u | 0x10000000u | 0x2 | 0x4 | 0x40000)) != 0;
        if (!detail::plain_file_name(name)) {
            set_last_error(ERROR_PATH_NOT_FOUND);
            return INVALID_HANDLE;
        }
        if (writes || (disposition != 3 && disposition != 4)) {
            set_last_error(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE;
        }
        u64 size = 0;
        i64 platform = detail::open_for_read(name, &size);
        if (platform < 0) {
            set_last_error(disposition == 4 ? ERROR_ACCESS_DENIED : ERROR_FILE_NOT_FOUND);
            return INVALID_HANDLE;
        }
        for (u32 i = 0; i < MAX_OPEN_FILES; ++i) {
            if (!g_extra.files[i].used) {
                g_extra.files[i] = OpenFile{ true, platform, size, 0, false, false, false, false };
                return FILE_HANDLE_BASE + 4ull * i;
            }
        }
        if (g_runtime->file_close) g_runtime->file_close(platform);
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE;
    }
    fsys::Resolved r;
    u32 e = fsys::resolve(name, &r);
    if (e) {
        set_last_error(e);
        return INVALID_HANDLE;
    }
    u32 slot = free_file_slot();
    if (slot == MAX_OPEN_FILES) {
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE;
    }
    bool writes = fsys::wants_write(access);
    if (r.program) {
        if (writes || r.length == 0 || (disposition != 3 && disposition != 4)) {
            set_last_error(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE;
        }
        u64 size = 0;
        i64 platform = detail::open_for_read(r.rel, &size);
        if (platform < 0) {
            set_last_error(disposition == 4 ? ERROR_ACCESS_DENIED : ERROR_FILE_NOT_FOUND);
            return INVALID_HANDLE;
        }
        g_extra.files[slot] = OpenFile{ true, platform, size, 0, false, false, false, false };
        return FILE_HANDLE_BASE + 4ull * slot;
    }
    fsys::Opened opened;
    e = fsys::open_path(r, access, disposition, flags, &opened);
    if (e) {
        set_last_error(e);
        return INVALID_HANDLE;
    }
    bool directory = opened.info.kind == FS_DIRECTORY;
    bool remove_later = (flags & fsys::FLAG_DELETE_ON_CLOSE) != 0 && !directory;
    g_extra.files[slot] = OpenFile{ true, opened.handle, opened.info.size, 0, true, writes, directory, remove_later };
    if (remove_later) detail::copy_text(g_extra.delete_paths[slot], sizeof(g_extra.delete_paths[slot]), r.rel);
    set_last_error(opened.existed && (disposition == fsys::CREATE_ALWAYS || disposition == fsys::OPEN_ALWAYS) ? fsys::E_ALREADY_EXISTS : 0);
    return FILE_HANDLE_BASE + 4ull * slot;
}

PEHOST_WINAPI inline u64 CreateFileW(const u16* name, u32 access, u32 share, void* security, u32 disposition, u32 flags, u64 model) {
    char narrow[MAX_NAME * 3 + 1];
    if (!detail::narrow_name(name, narrow)) {
        set_last_error(name == nullptr ? ERROR_INVALID_PARAMETER : ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE;
    }
    return CreateFileA(narrow, access, share, security, disposition, flags, model);
}

PEHOST_WINAPI inline i32 ReadFile(u64 handle, void* buffer, u32 length, u32* read, void* overlapped) {
    if (read) *read = 0;
    if (overlapped != nullptr) {
        set_last_error(ERROR_NOT_SUPPORTED);
        return 0;
    }
    if (handle == HANDLE_STDIN) {
        if (!g_runtime->read_stdin || length == 0 || buffer == nullptr) return 1;
        i64 n = g_runtime->read_stdin(buffer, length);
        if (n > 0) {
            if (read) *read = static_cast<u32>(n);
            return 1;
        }
        if (n < 0) {
            set_last_error(ERROR_BROKEN_PIPE);
            return 0;
        }
        return 1;
    }
    sync::Host guard;
    OpenFile* f = detail::file_of(handle);
    if (!f) {
        set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    if (f->work) {
        if (f->directory) {
            set_last_error(ERROR_INVALID_FUNCTION);
            return 0;
        }
        if (length == 0) return 1;
        i64 n = fsys::read_at(f->platform, f->position, buffer, length);
        if (n < 0) {
            set_last_error(fsys::translate(n, ERROR_ACCESS_DENIED));
            return 0;
        }
        f->position += static_cast<u64>(n);
        if (read) *read = static_cast<u32>(n);
        return 1;
    }
    if (f->position >= f->size || length == 0) return 1;
    u64 want = f->size - f->position;
    if (want > length) want = length;
    u64 got = 0;
    u8* out = static_cast<u8*>(buffer);
    while (got < want) {
        i64 n = g_runtime->file_read(f->platform, f->position + got, out + got, want - got);
        if (n <= 0) break;
        got += static_cast<u64>(n);
    }
    f->position += got;
    if (read) *read = static_cast<u32>(got);
    if (got != want) {
        set_last_error(ERROR_INVALID_FUNCTION);
        return 0;
    }
    return 1;
}

inline void refresh_size(OpenFile* f) {
    if (!f->work || !g_runtime->fs_stat) return;
    FsInfo info{};
    if (g_runtime->fs_stat(f->platform, &info) == 0) f->size = info.size;
}

PEHOST_WINAPI inline i32 GetFileSizeEx(u64 handle, i64* size) {
    sync::Host guard;
    OpenFile* f = detail::file_of(handle);
    if (!f || size == nullptr) {
        set_last_error(f ? ERROR_INVALID_PARAMETER : ERROR_INVALID_HANDLE);
        return 0;
    }
    refresh_size(f);
    *size = static_cast<i64>(f->size);
    return 1;
}

PEHOST_WINAPI inline u32 GetFileSize(u64 handle, u32* high) {
    i64 size = 0;
    if (!GetFileSizeEx(handle, &size)) return 0xFFFFFFFFu;
    if (high) *high = static_cast<u32>(static_cast<u64>(size) >> 32);
    else if (static_cast<u64>(size) >> 32) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0xFFFFFFFFu;
    }
    set_last_error(0);
    return static_cast<u32>(size);
}

PEHOST_WINAPI inline i32 SetFilePointerEx(u64 handle, i64 distance, i64* position, u32 method) {
    sync::Host guard;
    OpenFile* f = detail::file_of(handle);
    if (!f) {
        set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    i64 base;
    switch (method) {
        case 0: base = 0; break;
        case 1: base = static_cast<i64>(f->position); break;
        case 2:
            refresh_size(f);
            base = static_cast<i64>(f->size);
            break;
        default:
            set_last_error(ERROR_INVALID_PARAMETER);
            return 0;
    }
    i64 target = base + distance;
    if (target < 0) {
        set_last_error(ERROR_NEGATIVE_SEEK);
        return 0;
    }
    f->position = static_cast<u64>(target);
    if (position) *position = target;
    return 1;
}

PEHOST_WINAPI inline u32 SetFilePointer(u64 handle, i32 low, i32* high, u32 method) {
    i64 distance = high ? (static_cast<i64>(*high) << 32) | static_cast<u32>(low) : static_cast<i64>(low);
    i64 position = 0;
    if (!SetFilePointerEx(handle, distance, &position, method)) return 0xFFFFFFFFu;
    if (high) *high = static_cast<i32>(position >> 32);
    set_last_error(0);
    return static_cast<u32>(position);
}

PEHOST_WINAPI inline i32 SetEndOfFile(u64 handle) {
    sync::Host guard;
    OpenFile* f = detail::file_of(handle);
    if (!f) {
        set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    if (!f->work || !f->writable || f->directory || !g_runtime->fs_resize) {
        set_last_error(ERROR_ACCESS_DENIED);
        return 0;
    }
    i64 e = g_runtime->fs_resize(f->platform, f->position);
    if (e != 0) {
        set_last_error(fsys::translate(e, ERROR_ACCESS_DENIED));
        return 0;
    }
    f->size = f->position;
    return 1;
}

PEHOST_WINAPI inline i32 GetFileInformationByHandle(u64 handle, u8* data) {
    sync::Host guard;
    OpenFile* f = detail::file_of(handle);
    if (!f || data == nullptr) {
        set_last_error(f ? ERROR_INVALID_PARAMETER : ERROR_INVALID_HANDLE);
        return 0;
    }
    FsInfo info{};
    info.size = f->size;
    info.kind = f->directory ? FS_DIRECTORY : FS_FILE;
    if (f->work && g_runtime->fs_stat && g_runtime->fs_stat(f->platform, &info) == 0) f->size = info.size;
    for (u32 i = 0; i < 52; ++i) data[i] = 0;
    u64 stamp = detail::filetime_of(info.mtime_ms);
    *reinterpret_cast<u32*>(data) = info.kind == FS_DIRECTORY ? fsys::ATTRIBUTE_DIRECTORY : fsys::ATTRIBUTE_NORMAL;
    detail::put_filetime(data + 4, stamp);
    detail::put_filetime(data + 12, stamp);
    detail::put_filetime(data + 20, stamp);
    *reinterpret_cast<u32*>(data + 32) = static_cast<u32>(info.size >> 32);
    *reinterpret_cast<u32*>(data + 36) = static_cast<u32>(info.size);
    *reinterpret_cast<u32*>(data + 40) = 1;
    return 1;
}

PEHOST_WINAPI inline i32 GetFileTime(u64 handle, void* creation, void* access, void* written) {
    sync::Host guard;
    OpenFile* f = detail::file_of(handle);
    if (!f) {
        set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    FsInfo info{};
    if (!f->work || !g_runtime->fs_stat || g_runtime->fs_stat(f->platform, &info) != 0) {
        set_last_error(ERROR_NOT_SUPPORTED);
        return 0;
    }
    u64 stamp = detail::filetime_of(info.mtime_ms);
    if (creation) detail::put_filetime(creation, stamp);
    if (access) detail::put_filetime(access, stamp);
    if (written) detail::put_filetime(written, stamp);
    return 1;
}

inline u32 attributes_of(const char* name, u64* size, u64* mtime, bool* timed) {
    if (timed) *timed = false;
    if (!fsys::enabled()) {
        if (!detail::plain_file_name(name)) {
            set_last_error(ERROR_PATH_NOT_FOUND);
            return INVALID_FILE_ATTRIBUTES;
        }
        i64 platform = detail::open_for_read(name, size);
        if (platform < 0) {
            set_last_error(ERROR_FILE_NOT_FOUND);
            return INVALID_FILE_ATTRIBUTES;
        }
        if (g_runtime->file_close) g_runtime->file_close(platform);
        return 0x80;
    }
    fsys::Resolved r;
    u32 e = fsys::resolve(name, &r);
    if (e) {
        set_last_error(e);
        return INVALID_FILE_ATTRIBUTES;
    }
    if (r.program) {
        if (r.length == 0) return fsys::ATTRIBUTE_DIRECTORY;
        i64 platform = detail::open_for_read(r.rel, size);
        if (platform < 0) {
            set_last_error(ERROR_FILE_NOT_FOUND);
            return INVALID_FILE_ATTRIBUTES;
        }
        if (g_runtime->file_close) g_runtime->file_close(platform);
        return fsys::ATTRIBUTE_NORMAL;
    }
    FsInfo info{};
    if (r.length == 0) {
        i64 h = g_runtime->fs_open("", FS_WANT_LIST, &info);
        if (h < 0) return fsys::ATTRIBUTE_DIRECTORY;
        g_runtime->fs_close(h);
    } else if (!fsys::stat_path(r, &info)) {
        set_last_error(fsys::missing_error(r));
        return INVALID_FILE_ATTRIBUTES;
    }
    if (size) *size = info.kind == FS_DIRECTORY ? 0 : info.size;
    if (mtime) *mtime = info.mtime_ms;
    if (timed) *timed = true;
    return info.kind == FS_DIRECTORY ? fsys::ATTRIBUTE_DIRECTORY : fsys::ATTRIBUTE_NORMAL;
}

PEHOST_WINAPI inline u32 GetFileAttributesA(const char* name) {
    sync::Host guard;
    if (name == nullptr) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return INVALID_FILE_ATTRIBUTES;
    }
    u64 size = 0;
    return attributes_of(name, &size, nullptr, nullptr);
}

PEHOST_WINAPI inline u32 GetFileAttributesW(const u16* name) {
    char narrow[MAX_NAME * 3 + 1];
    if (!detail::narrow_name(name, narrow)) {
        set_last_error(name == nullptr ? ERROR_INVALID_PARAMETER : ERROR_PATH_NOT_FOUND);
        return INVALID_FILE_ATTRIBUTES;
    }
    return GetFileAttributesA(narrow);
}

PEHOST_WINAPI inline i32 GetFileAttributesExA(const char* name, u32 level, u8* data) {
    sync::Host guard;
    if (name == nullptr || data == nullptr || level != 0) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    u64 size = 0;
    u64 mtime = 0;
    bool timed = false;
    u32 attributes = attributes_of(name, &size, &mtime, &timed);
    if (attributes == INVALID_FILE_ATTRIBUTES) return 0;
    for (u32 i = 0; i < 36; ++i) data[i] = 0;
    *reinterpret_cast<u32*>(data) = attributes;
    if (timed) {
        u64 stamp = detail::filetime_of(mtime);
        detail::put_filetime(data + 4, stamp);
        detail::put_filetime(data + 12, stamp);
        detail::put_filetime(data + 20, stamp);
    }
    *reinterpret_cast<u32*>(data + 28) = static_cast<u32>(size >> 32);
    *reinterpret_cast<u32*>(data + 32) = static_cast<u32>(size);
    return 1;
}

PEHOST_WINAPI inline i32 GetFileAttributesExW(const u16* name, u32 level, u8* data) {
    char narrow[MAX_NAME * 3 + 1];
    if (!detail::narrow_name(name, narrow)) {
        set_last_error(name == nullptr ? ERROR_INVALID_PARAMETER : ERROR_PATH_NOT_FOUND);
        return 0;
    }
    return GetFileAttributesExA(narrow, level, data);
}

inline i32 deny() {
    set_last_error(ERROR_ACCESS_DENIED);
    return 0;
}

inline i32 finish(u32 error) {
    if (error == 0) return 1;
    set_last_error(error);
    return 0;
}

inline u32 prepare(const char* name, fsys::Resolved* r) {
    if (name == nullptr) return ERROR_INVALID_PARAMETER;
    return fsys::resolve(name, r);
}

PEHOST_WINAPI inline i32 DeleteFileA(const char* name) {
    sync::Host guard;
    if (!fsys::enabled()) return deny();
    fsys::Resolved r;
    u32 e = prepare(name, &r);
    return finish(e ? e : fsys::delete_file(r));
}

PEHOST_WINAPI inline i32 DeleteFileW(const u16* name) {
    char narrow[MAX_NAME * 3 + 1];
    if (!detail::narrow_name(name, narrow)) return finish(name == nullptr ? ERROR_INVALID_PARAMETER : ERROR_PATH_NOT_FOUND);
    return DeleteFileA(narrow);
}

PEHOST_WINAPI inline i32 RemoveDirectoryA(const char* name) {
    sync::Host guard;
    if (!fsys::enabled()) return deny();
    fsys::Resolved r;
    u32 e = prepare(name, &r);
    return finish(e ? e : fsys::remove_directory(r));
}

PEHOST_WINAPI inline i32 RemoveDirectoryW(const u16* name) {
    char narrow[MAX_NAME * 3 + 1];
    if (!detail::narrow_name(name, narrow)) return finish(name == nullptr ? ERROR_INVALID_PARAMETER : ERROR_PATH_NOT_FOUND);
    return RemoveDirectoryA(narrow);
}

PEHOST_WINAPI inline i32 CreateDirectoryA(const char* name, void*) {
    sync::Host guard;
    if (!fsys::enabled()) return deny();
    fsys::Resolved r;
    u32 e = prepare(name, &r);
    return finish(e ? e : fsys::make_directory(r));
}

PEHOST_WINAPI inline i32 CreateDirectoryW(const u16* name, void* security) {
    char narrow[MAX_NAME * 3 + 1];
    if (!detail::narrow_name(name, narrow)) return finish(name == nullptr ? ERROR_INVALID_PARAMETER : ERROR_PATH_NOT_FOUND);
    return CreateDirectoryA(narrow, security);
}

inline i32 move_names(const char* from, const char* to, u32 flags) {
    sync::Host guard;
    if (!fsys::enabled()) return deny();
    if (flags & ~0xBu) return finish(ERROR_NOT_SUPPORTED);
    if (flags & 0x4) return finish(ERROR_NOT_SUPPORTED);
    fsys::Resolved a, b;
    u32 e = prepare(from, &a);
    if (e == 0) e = prepare(to, &b);
    return finish(e ? e : fsys::move_path(a, b, (flags & 1) != 0));
}

PEHOST_WINAPI inline i32 MoveFileExA(const char* from, const char* to, u32 flags) { return move_names(from, to, flags); }

PEHOST_WINAPI inline i32 MoveFileA(const char* from, const char* to) { return move_names(from, to, 0); }

PEHOST_WINAPI inline i32 MoveFileExW(const u16* from, const u16* to, u32 flags) {
    char a[MAX_NAME * 3 + 1];
    char b[MAX_NAME * 3 + 1];
    if (!detail::narrow_name(from, a) || !detail::narrow_name(to, b)) return finish(from == nullptr || to == nullptr ? ERROR_INVALID_PARAMETER : ERROR_PATH_NOT_FOUND);
    return move_names(a, b, flags);
}

PEHOST_WINAPI inline i32 MoveFileW(const u16* from, const u16* to) { return MoveFileExW(from, to, 0); }

PEHOST_WINAPI inline i32 CopyFileA(const char* from, const char* to, i32 fail_if_exists) {
    sync::Host guard;
    if (!fsys::enabled()) return deny();
    fsys::Resolved a, b;
    u32 e = prepare(from, &a);
    if (e == 0) e = prepare(to, &b);
    return finish(e ? e : fsys::copy_file(a, b, fail_if_exists != 0));
}

PEHOST_WINAPI inline i32 CopyFileW(const u16* from, const u16* to, i32 fail_if_exists) {
    char a[MAX_NAME * 3 + 1];
    char b[MAX_NAME * 3 + 1];
    if (!detail::narrow_name(from, a) || !detail::narrow_name(to, b)) return finish(from == nullptr || to == nullptr ? ERROR_INVALID_PARAMETER : ERROR_PATH_NOT_FOUND);
    return CopyFileA(a, b, fail_if_exists);
}

PEHOST_WINAPI inline i32 SetFileAttributesA(const char* name, u32 attributes) {
    sync::Host guard;
    if (!fsys::enabled()) return deny();
    u64 size = 0;
    u32 now = attributes_of(name, &size, nullptr, nullptr);
    if (now == INVALID_FILE_ATTRIBUTES) return 0;
    if (attributes & 0x1) return finish(ERROR_NOT_SUPPORTED);
    return 1;
}

PEHOST_WINAPI inline i32 SetFileAttributesW(const u16* name, u32 attributes) {
    char narrow[MAX_NAME * 3 + 1];
    if (!detail::narrow_name(name, narrow)) return finish(name == nullptr ? ERROR_INVALID_PARAMETER : ERROR_PATH_NOT_FOUND);
    return SetFileAttributesA(narrow, attributes);
}

inline u32 root_directory(char* out, u32 cap) {
    if (cap < 2) return 2;
    out[0] = '\\';
    out[1] = 0;
    return 1;
}

inline u32 current_directory_text(char* out, u32 cap) {
    u32 n = 0;
    if (cap < 2) return 2;
    out[n++] = '\\';
    for (u32 i = 0; g_world.cwd[i]; ++i) {
        if (n + 1 >= cap) return n + 2;
        out[n++] = g_world.cwd[i] == '/' ? '\\' : g_world.cwd[i];
    }
    out[n] = 0;
    return n;
}

PEHOST_WINAPI inline u32 GetCurrentDirectoryA(u32 size, char* buffer) {
    sync::Host guard;
    char text[MAX_NAME + 4];
    u32 n = current_directory_text(text, sizeof(text));
    if (n + 1 > size) return n + 1;
    for (u32 i = 0; i <= n; ++i) buffer[i] = text[i];
    return n;
}

PEHOST_WINAPI inline u32 GetCurrentDirectoryW(u32 size, u16* buffer) {
    sync::Host guard;
    char text[MAX_NAME + 4];
    u32 n = current_directory_text(text, sizeof(text));
    if (n + 1 > size) return n + 1;
    for (u32 i = 0; i <= n; ++i) buffer[i] = static_cast<u8>(text[i]);
    return n;
}

PEHOST_WINAPI inline i32 SetCurrentDirectoryA(const char* name) {
    sync::Host guard;
    if (!fsys::enabled()) {
        if (name && name[0] == '\\' && name[1] == 0) return 1;
        set_last_error(ERROR_FILE_NOT_FOUND);
        return 0;
    }
    fsys::Resolved r;
    u32 e = prepare(name, &r);
    return finish(e ? e : fsys::change_directory(r));
}

PEHOST_WINAPI inline i32 SetCurrentDirectoryW(const u16* name) {
    if (!fsys::enabled()) {
        if (name && name[0] == '\\' && name[1] == 0) return 1;
        set_last_error(ERROR_FILE_NOT_FOUND);
        return 0;
    }
    char narrow[MAX_NAME * 3 + 1];
    if (!detail::narrow_name(name, narrow)) return finish(name == nullptr ? ERROR_INVALID_PARAMETER : ERROR_PATH_NOT_FOUND);
    return SetCurrentDirectoryA(narrow);
}

PEHOST_WINAPI inline u32 GetFullPathNameA(const char* name, u32 size, char* buffer, char** file_part) {
    if (name == nullptr || name[0] == 0) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    char full[MAX_NAME + 4];
    u32 n = 0;
    if (fsys::enabled()) {
        sync::Host guard;
        fsys::Resolved r;
        u32 e = fsys::resolve(name, &r, true);
        if (e) {
            set_last_error(e);
            return 0;
        }
        n = fsys::format_full(r, full, sizeof(full));
        if (n == 0) {
            set_last_error(ERROR_INVALID_PARAMETER);
            return 0;
        }
    } else {
        bool absolute = name[0] == '\\' || name[0] == '/' || (name[1] == ':' && name[0] != 0);
        if (!absolute) full[n++] = '\\';
        for (u32 i = 0; name[i] && n + 1 < sizeof(full); ++i) full[n++] = name[i];
        full[n] = 0;
    }
    if (n + 1 > size) return n + 1;
    for (u32 i = 0; i <= n; ++i) buffer[i] = full[i];
    if (file_part) {
        u32 at = 0;
        for (u32 i = 0; i < n; ++i) {
            if (full[i] == '\\' || full[i] == '/') at = i + 1;
        }
        *file_part = buffer + at;
    }
    return n;
}

PEHOST_WINAPI inline u32 GetFullPathNameW(const u16* name, u32 size, u16* buffer, u16** file_part) {
    char narrow[MAX_NAME * 3 + 1];
    if (!detail::narrow_name(name, narrow)) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    char full[MAX_NAME + 4];
    char* part = nullptr;
    u32 n = GetFullPathNameA(narrow, sizeof(full), full, &part);
    if (n == 0 || n >= sizeof(full)) return n;
    if (n + 1 > size) return n + 1;
    i64 w = codec::utf8_to_utf16(reinterpret_cast<const u8*>(full), n, buffer, size - 1, false);
    if (w < 0) return 0;
    buffer[w] = 0;
    if (file_part) {
        u32 at = 0;
        for (i64 i = 0; i < w; ++i) {
            if (buffer[i] == '\\' || buffer[i] == '/') at = static_cast<u32>(i) + 1;
        }
        *file_part = buffer + at;
    }
    return static_cast<u32>(w);
}

PEHOST_WINAPI inline u32 GetTempPathA(u32 size, char* buffer) {
    if (size < 2) return 2;
    buffer[0] = '\\';
    buffer[1] = 0;
    return 1;
}

PEHOST_WINAPI inline u64 LocalFree(u64 memory) {
    if (memory == 0) return 0;
    if (g_world.heap.free(reinterpret_cast<void*>(memory))) return 0;
    set_last_error(ERROR_INVALID_HANDLE);
    return memory;
}

inline EnvVar* env_find(const char* name) {
    for (EnvVar& e : g_extra.env) {
        if (e.used && detail::equal_folded(e.name, name)) return &e;
    }
    return nullptr;
}

inline bool env_name_ok(const char* name) {
    if (name == nullptr || name[0] == 0) return false;
    for (u32 i = 0; name[i]; ++i) {
        if (name[i] == '=' || i + 1 >= MAX_ENV_NAME) return false;
    }
    return true;
}

PEHOST_WINAPI inline i32 SetEnvironmentVariableA(const char* name, const char* value) {
    sync::Host guard;
    if (!env_name_ok(name) || (value && detail::text_length(value) + 1 > MAX_ENV_VALUE)) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    EnvVar* e = env_find(name);
    if (value == nullptr) {
        if (!e) {
            set_last_error(ERROR_ENVVAR_NOT_FOUND);
            return 0;
        }
        e->used = false;
        return 1;
    }
    if (!e) {
        for (EnvVar& slot : g_extra.env) {
            if (!slot.used) {
                e = &slot;
                break;
            }
        }
        if (!e) {
            set_last_error(ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        e->used = true;
        detail::copy_text(e->name, MAX_ENV_NAME, name);
    }
    detail::copy_text(e->value, MAX_ENV_VALUE, value);
    return 1;
}

PEHOST_WINAPI inline i32 SetEnvironmentVariableW(const u16* name, const u16* value) {
    sync::Host guard;
    char n[MAX_ENV_NAME * 3 + 1];
    char v[MAX_ENV_VALUE * 3 + 1];
    if (!detail::narrow_name(name, n)) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (value == nullptr) return SetEnvironmentVariableA(n, nullptr);
    u32 len = 0;
    while (value[len] && len < MAX_ENV_VALUE) ++len;
    if (value[len]) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    i64 w = codec::utf16_to_utf8(value, len, reinterpret_cast<u8*>(v), MAX_ENV_VALUE * 3, true);
    if (w < 0) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    v[w] = 0;
    return SetEnvironmentVariableA(n, v);
}

PEHOST_WINAPI inline u32 GetEnvironmentVariableA(const char* name, char* buffer, u32 size) {
    sync::Host guard;
    EnvVar* e = env_find(name ? name : "");
    if (!e) {
        set_last_error(ERROR_ENVVAR_NOT_FOUND);
        return 0;
    }
    u32 n = detail::text_length(e->value);
    if (n + 1 > size) return n + 1;
    for (u32 i = 0; i <= n; ++i) buffer[i] = e->value[i];
    return n;
}

PEHOST_WINAPI inline u32 GetEnvironmentVariableW(const u16* name, u16* buffer, u32 size) {
    sync::Host guard;
    char n[MAX_ENV_NAME * 3 + 1];
    if (!detail::narrow_name(name, n)) {
        set_last_error(ERROR_ENVVAR_NOT_FOUND);
        return 0;
    }
    EnvVar* e = env_find(n);
    if (!e) {
        set_last_error(ERROR_ENVVAR_NOT_FOUND);
        return 0;
    }
    u16 wide[MAX_ENV_VALUE];
    i64 w = codec::utf8_to_utf16(reinterpret_cast<const u8*>(e->value), detail::text_length(e->value), wide, MAX_ENV_VALUE - 1, false);
    if (w < 0) w = 0;
    if (static_cast<u64>(w) + 1 > size) return static_cast<u32>(w) + 1;
    for (i64 i = 0; i < w; ++i) buffer[i] = wide[i];
    buffer[w] = 0;
    return static_cast<u32>(w);
}

PEHOST_WINAPI inline u16* GetEnvironmentStringsW() {
    sync::Host guard;
    u64 units = 1;
    for (const EnvVar& e : g_extra.env) {
        if (e.used) units += detail::text_length(e.name) + detail::text_length(e.value) + 2;
    }
    u16* block = static_cast<u16*>(g_world.heap.alloc((units + 1) * sizeof(u16)));
    if (!block) {
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    u64 at = 0;
    for (const EnvVar& e : g_extra.env) {
        if (!e.used) continue;
        char line[MAX_ENV_NAME + MAX_ENV_VALUE + 2];
        u32 n = 0;
        for (u32 i = 0; e.name[i]; ++i) line[n++] = e.name[i];
        line[n++] = '=';
        for (u32 i = 0; e.value[i]; ++i) line[n++] = e.value[i];
        i64 w = codec::utf8_to_utf16(reinterpret_cast<const u8*>(line), n, block + at, units - at - 1, false);
        if (w < 0) continue;
        at += static_cast<u64>(w);
        block[at++] = 0;
    }
    block[at++] = 0;
    block[at] = 0;
    return block;
}

PEHOST_WINAPI inline i32 FreeEnvironmentStringsW(u16* block) { return block != nullptr && g_world.heap.free(block) ? 1 : 0; }

inline bool locale_ok(u32 locale) { return locale == 0x0409 || locale == 0x0400 || locale == 0x0800 || locale == 0x007F || locale == 0x0C00 || locale == 0; }

PEHOST_WINAPI inline u32 GetUserDefaultLCID() { return 0x0409; }

PEHOST_WINAPI inline u32 GetSystemDefaultLCID() { return 0x0409; }

PEHOST_WINAPI inline i32 IsValidLocale(u32 locale, u32) { return locale_ok(locale) ? 1 : 0; }

PEHOST_WINAPI inline i32 IsValidCodePage(u32 cp) { return cp == CODEPAGE_UTF8 ? 1 : 0; }

PEHOST_WINAPI inline i32 GetCPInfo(u32 cp, u8* info) {
    if (!detail::code_page_ok(cp) || info == nullptr) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    for (u32 i = 0; i < 20; ++i) info[i] = 0;
    *reinterpret_cast<u32*>(info) = 4;
    info[4] = '?';
    return 1;
}

PEHOST_WINAPI inline i32 EnumSystemLocalesW(i32 (PEHOST_WINAPI* callback)(u16*), u32) {
    if (callback == nullptr) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    static u16 name[] = { '0', '0', '0', '0', '0', '4', '0', '9', 0 };
    callback(name);
    return 1;
}

inline i32 locale_text(const char* text, u16* buffer, i32 size) {
    i32 n = static_cast<i32>(detail::text_length(text)) + 1;
    if (size == 0) return n;
    if (n > size) {
        set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (i32 i = 0; i < n; ++i) buffer[i] = static_cast<u16>(static_cast<u8>(text[i]));
    return n;
}

PEHOST_WINAPI inline i32 GetLocaleInfoW(u32 locale, u32 type, u16* buffer, i32 size) {
    if (!locale_ok(locale) || size < 0) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    bool number = (type & 0x20000000u) != 0;
    type &= ~(0x80000000u | 0x20000000u);
    if (number) {
        u32 value;
        switch (type) {
            case 0x0B:
            case 0x1004: value = CODEPAGE_UTF8; break;
            case 0x01: value = 0x0409; break;
            default:
                set_last_error(ERROR_INVALID_FLAGS);
                return 0;
        }
        if (size < 2) {
            if (size == 0) return 2;
            set_last_error(ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        *reinterpret_cast<u32*>(buffer) = value;
        return 2;
    }
    const char* text;
    switch (type) {
        case 0x0E: text = "."; break;
        case 0x0F: text = ","; break;
        case 0x10: text = "3;0"; break;
        case 0x0B: text = "65001"; break;
        case 0x1004: text = "65001"; break;
        case 0x59: text = "en"; break;
        case 0x5A: text = "US"; break;
        case 0x5C: text = "en-US"; break;
        case 0x1001: text = "English"; break;
        case 0x1002: text = "United States"; break;
        default:
            set_last_error(ERROR_INVALID_FLAGS);
            return 0;
    }
    return locale_text(text, buffer, size);
}

PEHOST_WINAPI inline i32 GetUserDefaultLocaleName(u16* buffer, i32 size) { return locale_text("en-US", buffer, size); }

inline i32 compare_units(u32 flags, const u16* a, i32 an, const u16* b, i32 bn) {
    if ((flags & ~(0x1u | 0x10u | 0x1000u)) != 0) {
        set_last_error(ERROR_INVALID_FLAGS);
        return 0;
    }
    if (a == nullptr || b == nullptr) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (an < 0) {
        an = 0;
        while (a[an]) ++an;
    }
    if (bn < 0) {
        bn = 0;
        while (b[bn]) ++bn;
    }
    bool fold_case = (flags & 0x11) != 0;
    i32 n = an < bn ? an : bn;
    for (i32 i = 0; i < n; ++i) {
        u16 x = fold_case ? detail::upper16(a[i]) : a[i];
        u16 y = fold_case ? detail::upper16(b[i]) : b[i];
        if (x != y) return x < y ? 1 : 3;
    }
    return an == bn ? 2 : (an < bn ? 1 : 3);
}

PEHOST_WINAPI inline i32 CompareStringW(u32, u32 flags, const u16* a, i32 an, const u16* b, i32 bn) { return compare_units(flags, a, an, b, bn); }

PEHOST_WINAPI inline i32 CompareStringEx(const u16*, u32 flags, const u16* a, i32 an, const u16* b, i32 bn, void*, void*, u64) {
    return compare_units(flags, a, an, b, bn);
}

inline i32 map_units(u32 flags, const u16* src, i32 count, u16* dst, i32 capacity) {
    u32 kind = flags & ~0x400000u;
    if (kind != 0x100 && kind != 0x200) {
        set_last_error(kind == 0 ? ERROR_INVALID_FLAGS : ERROR_NOT_SUPPORTED);
        return 0;
    }
    if (src == nullptr || count == 0 || capacity < 0) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (count < 0) {
        count = 0;
        while (src[count]) ++count;
        ++count;
    }
    if (capacity == 0) return count;
    if (capacity < count) {
        set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (i32 i = 0; i < count; ++i) dst[i] = kind == 0x100 ? detail::lower16(src[i]) : detail::upper16(src[i]);
    return count;
}

PEHOST_WINAPI inline i32 LCMapStringW(u32, u32 flags, const u16* src, i32 count, u16* dst, i32 capacity) { return map_units(flags, src, count, dst, capacity); }

PEHOST_WINAPI inline i32 LCMapStringEx(const u16*, u32 flags, const u16* src, i32 count, u16* dst, i32 capacity, void*, void*, u64) {
    return map_units(flags, src, count, dst, capacity);
}

PEHOST_WINAPI inline i32 GetStringTypeW(u32 info, const u16* src, i32 count, u16* out) {
    if (info != 1 || src == nullptr || out == nullptr || count == 0) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (count < 0) {
        count = 0;
        while (src[count]) ++count;
        ++count;
    }
    for (i32 i = 0; i < count; ++i) {
        u16 c = src[i];
        u16 t = 0;
        if (c < 0x80) {
            if (c >= 'A' && c <= 'Z') t = 0x1 | 0x100 | (c <= 'F' ? 0x80 : 0);
            else if (c >= 'a' && c <= 'z') t = 0x2 | 0x100 | (c <= 'f' ? 0x80 : 0);
            else if (c >= '0' && c <= '9') t = 0x4 | 0x80;
            else if (c == ' ') t = 0x8 | 0x40;
            else if (c == '\t') t = 0x8 | 0x20 | 0x40;
            else if (c == '\n' || c == '\v' || c == '\f' || c == '\r') t = 0x8 | 0x20;
            else if (c < 0x20 || c == 0x7F) t = 0x20;
            else t = 0x10;
        }
        out[i] = t;
    }
    return 1;
}

}

}

namespace pehost {

constexpr u32 MAX_SEARCHES = 8;
constexpr u64 SEARCH_HANDLE_BASE = 0x1000;
constexpr u32 ERROR_NO_MORE_FILES = 18;
constexpr u32 ERROR_MR_MID_NOT_FOUND = 317;

struct Search {
    bool used;
    bool wide;
    bool program;
    u64 next;
    char dir[fsys::TEXT_MAX + 1];
    char pattern[MAX_NAME + 1];
};

inline Search g_searches[MAX_SEARCHES];

namespace detail {

inline bool wildcard_match(const char* pattern, const char* name) {
    while (*pattern) {
        if (*pattern == '*') {
            while (*pattern == '*') ++pattern;
            if (*pattern == 0) return true;
            for (const char* n = name;; ++n) {
                if (wildcard_match(pattern, n)) return true;
                if (*n == 0) return false;
            }
        }
        if (*name == 0) return false;
        char a = fold(*pattern), b = fold(*name);
        if (*pattern != '?' && a != b) return false;
        ++pattern;
        ++name;
    }
    return *name == 0;
}

}

namespace win32 {

inline bool fill_find_data(const Search& s, u8* data, const char* name, u64 size, u64 mtime, bool directory) {
    u32 n = detail::text_length(name);
    for (u32 i = 0; i < (s.wide ? 592u : 320u); ++i) data[i] = 0;
    u64 stamp = detail::filetime_of(mtime);
    *reinterpret_cast<u32*>(data) = directory ? fsys::ATTRIBUTE_DIRECTORY : fsys::ATTRIBUTE_NORMAL;
    detail::put_filetime(data + 4, stamp);
    detail::put_filetime(data + 12, stamp);
    detail::put_filetime(data + 20, stamp);
    *reinterpret_cast<u32*>(data + 28) = static_cast<u32>(size >> 32);
    *reinterpret_cast<u32*>(data + 32) = static_cast<u32>(size);
    if (s.wide) {
        i64 w = codec::utf8_to_utf16(reinterpret_cast<const u8*>(name), n, reinterpret_cast<u16*>(data + 44), 259, false);
        if (w < 0) return false;
        reinterpret_cast<u16*>(data + 44)[w] = 0;
        return true;
    }
    if (n > 259) return false;
    for (u32 i = 0; i <= n; ++i) data[44 + i] = static_cast<u8>(name[i]);
    return true;
}

inline bool search_step_program(Search& s, u8* data) {
    if (!g_runtime->dir_entry) return false;
    for (;;) {
        char name[256];
        u64 size = 0, mtime = 0;
        bool directory = false;
        i32 r = g_runtime->dir_entry(s.next++, name, sizeof(name), &size, &mtime, &directory);
        if (r <= 0) return false;
        if (!detail::wildcard_match(s.pattern, name)) continue;
        if (fill_find_data(s, data, name, size, mtime, directory)) return true;
    }
}

inline bool search_step_work(Search& s, u8* data) {
    FsInfo info{};
    i64 h = g_runtime->fs_open(s.dir, FS_WANT_LIST, &info);
    if (h < 0) return false;
    if (info.kind != FS_DIRECTORY) {
        g_runtime->fs_close(h);
        return false;
    }
    bool found = false;
    for (;;) {
        char name[256];
        FsInfo entry{};
        if (g_runtime->fs_entry(h, s.next++, name, sizeof(name), &entry) != 0) break;
        if (!detail::wildcard_match(s.pattern, name)) continue;
        if (fill_find_data(s, data, name, entry.size, entry.mtime_ms, entry.kind == FS_DIRECTORY)) {
            found = true;
            break;
        }
    }
    g_runtime->fs_close(h);
    return found;
}

inline bool search_step(Search& s, u8* data) {
    return s.program ? search_step_program(s, data) : search_step_work(s, data);
}

inline bool search_pattern(const char* text, char* out) {
    if (text == nullptr) return false;
    if (text[0] == '.' && (text[1] == '\\' || text[1] == '/')) text += 2;
    else if (text[0] == '\\' || text[0] == '/') text += 1;
    if (text[0] == 0) return false;
    u32 n = 0;
    for (; text[n]; ++n) {
        if (n >= MAX_NAME || text[n] == '\\' || text[n] == '/' || text[n] == ':') return false;
        out[n] = text[n];
    }
    out[n] = 0;
    if (n == 3 && out[0] == '*' && out[1] == '.' && out[2] == '*') out[1] = 0;
    return true;
}

inline u32 split_search(const char* text, Search& s) {
    if (text == nullptr || text[0] == 0) return ERROR_PATH_NOT_FOUND;
    u32 n = fsys::length_of(text);
    if (n > fsys::TEXT_MAX) return fsys::E_INVALID_NAME;
    u32 split = n;
    while (split > 0 && !fsys::is_separator(text[split - 1])) --split;
    if (split == 0 && n >= 2 && text[1] == ':') split = 2;
    char directory[fsys::TEXT_MAX + 2];
    if (split == 0) {
        directory[0] = '.';
        directory[1] = 0;
    } else {
        for (u32 i = 0; i < split; ++i) directory[i] = text[i];
        directory[split] = 0;
    }
    const char* leaf = text + split;
    u32 leaf_length = n - split;
    if (leaf_length == 0) return fsys::E_INVALID_NAME;
    bool dots = true;
    for (u32 i = 0; i < leaf_length; ++i) {
        if (leaf[i] != '.') dots = false;
    }
    if (dots) return ERROR_FILE_NOT_FOUND;
    if (!fsys::component_ok(leaf, leaf_length, true)) return fsys::E_INVALID_NAME;
    fsys::Resolved r;
    u32 e = fsys::resolve(directory, &r);
    if (e) return e == fsys::E_INVALID_NAME ? e : ERROR_PATH_NOT_FOUND;
    if (r.program) {
        if (r.length != 0) return ERROR_PATH_NOT_FOUND;
        s.program = true;
        s.dir[0] = 0;
    } else {
        FsInfo info{};
        if (!fsys::stat_path(r, &info) || info.kind != FS_DIRECTORY) return ERROR_PATH_NOT_FOUND;
        s.program = false;
        detail::copy_text(s.dir, sizeof(s.dir), r.rel);
    }
    detail::copy_text(s.pattern, sizeof(s.pattern), leaf);
    if (leaf_length == 3 && leaf[0] == '*' && leaf[1] == '.' && leaf[2] == '*') s.pattern[1] = 0;
    return 0;
}

inline u64 begin_search(const char* pattern, u8* data, bool wide) {
    sync::Host guard;
    Search probe{};
    probe.wide = wide;
    if (fsys::enabled()) {
        u32 e = split_search(pattern, probe);
        if (e) {
            set_last_error(e);
            return INVALID_HANDLE;
        }
    } else {
        char plain[MAX_NAME + 1];
        if (!search_pattern(pattern, plain)) {
            set_last_error(ERROR_PATH_NOT_FOUND);
            return INVALID_HANDLE;
        }
        probe.program = true;
        detail::copy_text(probe.pattern, sizeof(probe.pattern), plain);
    }
    for (u32 i = 0; i < MAX_SEARCHES; ++i) {
        if (g_searches[i].used) continue;
        g_searches[i] = probe;
        g_searches[i].used = true;
        g_searches[i].next = 0;
        if (!search_step(g_searches[i], data)) {
            g_searches[i].used = false;
            set_last_error(ERROR_FILE_NOT_FOUND);
            return INVALID_HANDLE;
        }
        return SEARCH_HANDLE_BASE + 4ull * i;
    }
    set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return INVALID_HANDLE;
}

inline Search* search_of(u64 handle, bool wide) {
    if (handle < SEARCH_HANDLE_BASE || (handle - SEARCH_HANDLE_BASE) % 4 != 0) return nullptr;
    u64 i = (handle - SEARCH_HANDLE_BASE) / 4;
    if (i >= MAX_SEARCHES || !g_searches[i].used || g_searches[i].wide != wide) return nullptr;
    return &g_searches[i];
}

PEHOST_WINAPI inline u64 FindFirstFileExA(const char* pattern, u32 level, u8* data, u32 operation, void*, u32) {
    if (data == nullptr || level > 1 || operation != 0) {
        set_last_error(operation > 2 || level > 1 || data == nullptr ? ERROR_INVALID_PARAMETER : ERROR_NOT_SUPPORTED);
        return INVALID_HANDLE;
    }
    return begin_search(pattern, data, false);
}

PEHOST_WINAPI inline u64 FindFirstFileExW(const u16* pattern, u32 level, u8* data, u32 operation, void*, u32) {
    char narrow[MAX_NAME * 3 + 1];
    if (data == nullptr || level > 1 || operation != 0 || !detail::narrow_name(pattern, narrow)) {
        set_last_error(operation > 2 || level > 1 || data == nullptr ? ERROR_INVALID_PARAMETER : (operation != 0 ? ERROR_NOT_SUPPORTED : ERROR_PATH_NOT_FOUND));
        return INVALID_HANDLE;
    }
    return begin_search(narrow, data, true);
}

PEHOST_WINAPI inline u64 FindFirstFileA(const char* pattern, u8* data) { return FindFirstFileExA(pattern, 0, data, 0, nullptr, 0); }

PEHOST_WINAPI inline u64 FindFirstFileW(const u16* pattern, u8* data) { return FindFirstFileExW(pattern, 0, data, 0, nullptr, 0); }

PEHOST_WINAPI inline i32 FindNextFileA(u64 handle, u8* data) {
    sync::Host guard;
    Search* s = search_of(handle, false);
    if (!s || data == nullptr) {
        set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    if (!search_step(*s, data)) {
        set_last_error(ERROR_NO_MORE_FILES);
        return 0;
    }
    return 1;
}

PEHOST_WINAPI inline i32 FindNextFileW(u64 handle, u8* data) {
    sync::Host guard;
    Search* s = search_of(handle, true);
    if (!s || data == nullptr) {
        set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    if (!search_step(*s, data)) {
        set_last_error(ERROR_NO_MORE_FILES);
        return 0;
    }
    return 1;
}

PEHOST_WINAPI inline i32 FindClose(u64 handle) {
    sync::Host guard;
    if (handle < SEARCH_HANDLE_BASE || (handle - SEARCH_HANDLE_BASE) % 4 != 0 || (handle - SEARCH_HANDLE_BASE) / 4 >= MAX_SEARCHES ||
        !g_searches[(handle - SEARCH_HANDLE_BASE) / 4].used) {
        set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    g_searches[(handle - SEARCH_HANDLE_BASE) / 4].used = false;
    return 1;
}

PEHOST_WINAPI inline u64 VerSetConditionMask(u64 mask, u32 types, u8 condition) {
    for (u32 bit = 0; bit < 8; ++bit) {
        if (types & (1u << bit)) mask |= static_cast<u64>(condition & 7) << (bit * 3);
    }
    return mask;
}

inline i32 version_fail() {
    set_last_error(ERROR_OLD_WIN_VERSION);
    return 0;
}

inline bool version_test(u32 actual, u32 wanted, u32 condition) {
    switch (condition) {
        case 1: return actual == wanted;
        case 2: return actual > wanted;
        case 3: return actual >= wanted;
        case 4: return actual < wanted;
        case 5: return actual <= wanted;
        default: return false;
    }
}

inline u32 condition_of(u64 mask, u32 bit) {
    u32 index = 0;
    while (!((bit >> index) & 1)) ++index;
    return static_cast<u32>((mask >> (index * 3)) & 7);
}

PEHOST_WINAPI inline i32 VerifyVersionInfoA(const u8* info, u32 types, u64 mask) {
    if (info == nullptr || (types & ~0xFFu) != 0 || types == 0 || *reinterpret_cast<const u32*>(info) < ((types & 0xF0u) ? 156u : 148u)) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    struct Field {
        u32 bit;
        u32 actual;
        u32 wanted;
    };
    Field hierarchy[4] = {
        { 0x2, 10, *reinterpret_cast<const u32*>(info + 4) },
        { 0x1, 0, *reinterpret_cast<const u32*>(info + 8) },
        { 0x20, 0, *reinterpret_cast<const u16*>(info + 148) },
        { 0x10, 0, *reinterpret_cast<const u16*>(info + 150) },
    };
    i32 last = -1;
    for (i32 i = 0; i < 4; ++i) {
        if (types & hierarchy[i].bit) last = i;
    }
    bool decided = false;
    for (i32 i = 0; i < 4 && !decided; ++i) {
        const Field& f = hierarchy[i];
        if (!(types & f.bit)) continue;
        u32 condition = condition_of(mask, f.bit);
        if (condition == 1) {
            if (f.actual != f.wanted) return version_fail();
        } else if (condition == 2 || condition == 3) {
            if (f.actual > f.wanted) decided = true;
            else if (f.actual < f.wanted || (condition == 2 && i == last)) return version_fail();
        } else if (condition == 4 || condition == 5) {
            if (f.actual < f.wanted) decided = true;
            else if (f.actual > f.wanted || (condition == 4 && i == last)) return version_fail();
        } else {
            return version_fail();
        }
    }
    Field flat[3] = {
        { 0x4, 19045, *reinterpret_cast<const u32*>(info + 12) },
        { 0x8, 2, *reinterpret_cast<const u32*>(info + 16) },
        { 0x80, 1, info[154] },
    };
    for (const Field& f : flat) {
        if ((types & f.bit) && !version_test(f.actual, f.wanted, condition_of(mask, f.bit))) return version_fail();
    }
    if (types & 0x40) {
        u32 wanted = *reinterpret_cast<const u16*>(info + 152);
        if (wanted != 0) return version_fail();
    }
    return 1;
}

PEHOST_WINAPI inline u32 FormatMessageA(u32 flags, const void*, u32 id, u32, char* buffer, u32 size, void*) {
    if ((flags & ~(0x100u | 0x200u | 0x1000u | 0xFFu)) != 0 || !(flags & 0x1000)) {
        set_last_error((flags & (0x400u | 0x800u)) ? ERROR_NOT_SUPPORTED : ERROR_INVALID_PARAMETER);
        return 0;
    }
    const char* text;
    switch (id) {
        case 2: text = "The system cannot find the file specified.\r\n"; break;
        case 3: text = "The system cannot find the path specified.\r\n"; break;
        case 5: text = "Access is denied.\r\n"; break;
        case 6: text = "The handle is invalid.\r\n"; break;
        case 8: text = "Not enough memory resources are available to process this command.\r\n"; break;
        case 50: text = "The request is not supported.\r\n"; break;
        case 87: text = "The parameter is incorrect.\r\n"; break;
        case 122: text = "The data area passed to a system call is too small.\r\n"; break;
        case 126: text = "The specified module could not be found.\r\n"; break;
        case 127: text = "The specified procedure could not be found.\r\n"; break;
        case 1114: text = "A dynamic link library (DLL) initialization routine failed.\r\n"; break;
        case 1460: text = "This operation returned because the timeout period expired.\r\n"; break;
        default:
            set_last_error(ERROR_MR_MID_NOT_FOUND);
            return 0;
    }
    u32 n = detail::text_length(text);
    if (flags & 0x100) {
        char* block = static_cast<char*>(g_world.heap.alloc(n + 1));
        if (!block) {
            set_last_error(ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        for (u32 i = 0; i <= n; ++i) block[i] = text[i];
        *reinterpret_cast<char**>(buffer) = block;
        return n;
    }
    if (buffer == nullptr || size < n + 1) {
        set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (u32 i = 0; i <= n; ++i) buffer[i] = text[i];
    return n;
}

}

}

namespace pehost::win32 {

inline i32 unsupported_false() {
    set_last_error(ERROR_NOT_SUPPORTED);
    return 0;
}

inline u64 unsupported_handle() {
    set_last_error(ERROR_NOT_SUPPORTED);
    return INVALID_HANDLE;
}

PEHOST_WINAPI inline u64 CreateNamedPipeA(const char*, u32, u32, u32, u32, u32, u32, void*) { return unsupported_handle(); }
PEHOST_WINAPI inline i32 ConnectNamedPipe(u64, void*) { return unsupported_false(); }
PEHOST_WINAPI inline i32 CreatePipe(u64*, u64*, void*, u32) { return unsupported_false(); }
PEHOST_WINAPI inline i32 CreateProcessA(const char*, char*, void*, void*, i32, u32, void*, const char*, void*, void*) { return unsupported_false(); }
PEHOST_WINAPI inline i32 SetHandleInformation(u64, u32, u32) { return unsupported_false(); }
PEHOST_WINAPI inline u64 CreateIoCompletionPort(u64, u64, u64, u32) {
    set_last_error(ERROR_NOT_SUPPORTED);
    return 0;
}
PEHOST_WINAPI inline i32 GetQueuedCompletionStatus(u64, u32*, u64*, void**, u32) { return unsupported_false(); }
PEHOST_WINAPI inline i32 PostQueuedCompletionStatus(u64, u32, u64, void*) { return unsupported_false(); }
PEHOST_WINAPI inline i32 GetOverlappedResult(u64, void*, u32*, i32) { return unsupported_false(); }
PEHOST_WINAPI inline i32 GenerateConsoleCtrlEvent(u32, u32) { return unsupported_false(); }
PEHOST_WINAPI inline i32 QueryInformationJobObject(u64, u32, void*, u32, u32*) { return unsupported_false(); }
PEHOST_WINAPI inline i32 GetSystemTimes(u64*, u64*, u64*) { return unsupported_false(); }
PEHOST_WINAPI inline i32 WriteConsoleOutputA(u64, const void*, u32, u32, void*) { return unsupported_false(); }
PEHOST_WINAPI inline u32 GetProcessId(u64) {
    set_last_error(ERROR_NOT_SUPPORTED);
    return 0;
}
PEHOST_WINAPI inline i32 GetExitCodeProcess(u64, u32*) { return unsupported_false(); }
PEHOST_WINAPI inline u32 GetDriveTypeW(const u16*) { return 1; }

}

namespace pehost {

inline const ImportTable* kernel32_stub_table() {
#define PEHOST_S(name) { "kernel32.dll", #name, reinterpret_cast<void*>(&win32::name) }
    static const Import items[] = {
        PEHOST_S(CreateNamedPipeA), PEHOST_S(ConnectNamedPipe), PEHOST_S(CreatePipe), PEHOST_S(CreateProcessA), 
        PEHOST_S(SetHandleInformation), PEHOST_S(CreateIoCompletionPort), PEHOST_S(GetQueuedCompletionStatus), PEHOST_S(PostQueuedCompletionStatus),
        PEHOST_S(GetOverlappedResult), PEHOST_S(GenerateConsoleCtrlEvent),
        PEHOST_S(QueryInformationJobObject), PEHOST_S(GetSystemTimes), PEHOST_S(WriteConsoleOutputA), PEHOST_S(GetProcessId),
        PEHOST_S(GetExitCodeProcess), PEHOST_S(GetDriveTypeW),
    };
#undef PEHOST_S
    static const ImportTable table{ items, sizeof(items) / sizeof(items[0]), kernel32_table() };
    return &table;
}

}

namespace pehost {

inline const ImportTable* kernel32x_table() {
#define PEHOST_K(name) { "kernel32.dll", #name, reinterpret_cast<void*>(&win32::name) }
    static const Import items[] = {
        PEHOST_K(QueryPerformanceFrequency),
        PEHOST_K(QueryPerformanceCounter),
        PEHOST_K(GetSystemTimeAsFileTime),
        PEHOST_K(GetTickCount64),
        PEHOST_K(GetTickCount),
        PEHOST_K(IsDebuggerPresent),
        PEHOST_K(IsProcessorFeaturePresent),
        PEHOST_K(GetActiveProcessorCount),
        PEHOST_K(GetStartupInfoW),
        PEHOST_K(GetStartupInfoA),
        PEHOST_K(EncodePointer),
        PEHOST_K(DecodePointer),
        PEHOST_K(InitializeSListHead),
        PEHOST_K(InterlockedPushEntrySList),
        PEHOST_K(InterlockedPopEntrySList),
        PEHOST_K(InterlockedFlushSList),
        PEHOST_K(QueryDepthSList),
        PEHOST_K(InitializeCriticalSectionEx),
        PEHOST_K(InitializeCriticalSectionAndSpinCount),
        PEHOST_K(SetCriticalSectionSpinCount),
        PEHOST_K(FlsAlloc),
        PEHOST_K(FlsFree),
        PEHOST_K(FlsGetValue),
        PEHOST_K(FlsSetValue),
        PEHOST_K(InitializeSRWLock),
        PEHOST_K(AcquireSRWLockExclusive),
        PEHOST_K(ReleaseSRWLockExclusive),
        PEHOST_K(TryAcquireSRWLockExclusive),
        PEHOST_K(AcquireSRWLockShared),
        PEHOST_K(ReleaseSRWLockShared),
        PEHOST_K(TryAcquireSRWLockShared),
        PEHOST_K(InitializeConditionVariable),
        PEHOST_K(WakeConditionVariable),
        PEHOST_K(WakeAllConditionVariable),
        PEHOST_K(SleepConditionVariableSRW),
        PEHOST_K(SleepConditionVariableCS),
        PEHOST_K(CreateEventA),
        PEHOST_K(CreateEventW),
        PEHOST_K(CreateEventExA),
        PEHOST_K(CreateEventExW),
        PEHOST_K(OpenEventA),
        PEHOST_K(OpenEventW),
        PEHOST_K(SetEvent),
        PEHOST_K(ResetEvent),
        PEHOST_K(CreateMutexA),
        PEHOST_K(CreateMutexW),
        PEHOST_K(CreateMutexExA),
        PEHOST_K(CreateMutexExW),
        PEHOST_K(OpenMutexA),
        PEHOST_K(OpenMutexW),
        PEHOST_K(ReleaseMutex),
        PEHOST_K(CreateSemaphoreA),
        PEHOST_K(CreateSemaphoreW),
        PEHOST_K(CreateSemaphoreExA),
        PEHOST_K(CreateSemaphoreExW),
        PEHOST_K(OpenSemaphoreA),
        PEHOST_K(OpenSemaphoreW),
        PEHOST_K(ReleaseSemaphore),
        PEHOST_K(WaitForSingleObject),
        PEHOST_K(WaitForSingleObjectEx),
        PEHOST_K(WaitForMultipleObjects),
        PEHOST_K(WaitForMultipleObjectsEx),
        PEHOST_K(DuplicateHandle),
        PEHOST_K(InitOnceBeginInitialize),
        PEHOST_K(InitOnceComplete),
        PEHOST_K(InitOnceExecuteOnce),
        PEHOST_K(InitOnceInitialize),
        PEHOST_K(WaitOnAddress),
        PEHOST_K(WakeByAddressSingle),
        PEHOST_K(WakeByAddressAll),
        PEHOST_K(RtlCaptureContext),
        PEHOST_K(RtlLookupFunctionEntry),
        PEHOST_K(RtlPcToFileHeader),
        PEHOST_K(RaiseException),
        PEHOST_K(UnhandledExceptionFilter),
        PEHOST_K(RtlUnwind),
        PEHOST_K(RtlUnwindEx),
        PEHOST_K(RtlVirtualUnwind),
        PEHOST_K(GetModuleFileNameW),
        PEHOST_K(LoadLibraryExA),
        PEHOST_K(LoadLibraryExW),
        PEHOST_K(GetModuleHandleExW),
        PEHOST_K(GetFileType),
        PEHOST_K(GetConsoleMode),
        PEHOST_K(SetConsoleMode),
        PEHOST_K(GetConsoleScreenBufferInfo),
        PEHOST_K(GetConsoleOutputCP),
        PEHOST_K(GetConsoleCP),
        PEHOST_K(GetOEMCP),
        PEHOST_K(SetConsoleCtrlHandler),
        PEHOST_K(SetStdHandle),
        PEHOST_K(WriteConsoleW),
        PEHOST_K(ReadConsoleW),
        PEHOST_K(FlushFileBuffers),
        PEHOST_K(CloseHandle),
        PEHOST_K(CreateFileA),
        PEHOST_K(CreateFileW),
        PEHOST_K(ReadFile),
        PEHOST_K(GetFileSizeEx),
        PEHOST_K(GetFileSize),
        PEHOST_K(SetFilePointerEx),
        PEHOST_K(SetFilePointer),
        PEHOST_K(SetEndOfFile),
        PEHOST_K(GetFileInformationByHandle),
        PEHOST_K(GetFileTime),
        PEHOST_K(GetFileAttributesA),
        PEHOST_K(GetFileAttributesW),
        PEHOST_K(GetFileAttributesExA),
        PEHOST_K(GetFileAttributesExW),
        PEHOST_K(DeleteFileA),
        PEHOST_K(DeleteFileW),
        PEHOST_K(RemoveDirectoryA),
        PEHOST_K(RemoveDirectoryW),
        PEHOST_K(CreateDirectoryA),
        PEHOST_K(CreateDirectoryW),
        PEHOST_K(MoveFileA),
        PEHOST_K(MoveFileW),
        PEHOST_K(MoveFileExA),
        PEHOST_K(MoveFileExW),
        PEHOST_K(CopyFileA),
        PEHOST_K(CopyFileW),
        PEHOST_K(SetFileAttributesA),
        PEHOST_K(SetFileAttributesW),
        PEHOST_K(GetCurrentDirectoryA),
        PEHOST_K(GetCurrentDirectoryW),
        PEHOST_K(SetCurrentDirectoryA),
        PEHOST_K(SetCurrentDirectoryW),
        PEHOST_K(GetFullPathNameA),
        PEHOST_K(GetFullPathNameW),
        PEHOST_K(GetTempPathA),
        PEHOST_K(LocalFree),
        PEHOST_K(SetEnvironmentVariableA),
        PEHOST_K(SetEnvironmentVariableW),
        PEHOST_K(GetEnvironmentVariableA),
        PEHOST_K(GetEnvironmentVariableW),
        PEHOST_K(GetEnvironmentStringsW),
        PEHOST_K(FreeEnvironmentStringsW),
        PEHOST_K(GetUserDefaultLCID),
        PEHOST_K(GetSystemDefaultLCID),
        PEHOST_K(IsValidLocale),
        PEHOST_K(IsValidCodePage),
        PEHOST_K(GetCPInfo),
        PEHOST_K(EnumSystemLocalesW),
        PEHOST_K(GetLocaleInfoW),
        PEHOST_K(GetUserDefaultLocaleName),
        PEHOST_K(CompareStringW),
        PEHOST_K(CompareStringEx),
        PEHOST_K(LCMapStringW),
        PEHOST_K(LCMapStringEx),
        PEHOST_K(GetStringTypeW),
        PEHOST_K(FindFirstFileExA),
        PEHOST_K(FindFirstFileExW),
        PEHOST_K(FindFirstFileA),
        PEHOST_K(FindFirstFileW),
        PEHOST_K(FindNextFileA),
        PEHOST_K(FindNextFileW),
        PEHOST_K(FindClose),
        PEHOST_K(VerSetConditionMask),
        PEHOST_K(VerifyVersionInfoA),
        PEHOST_K(FormatMessageA),
    };
#undef PEHOST_K
    static const ImportTable table{ items, sizeof(items) / sizeof(items[0]), kernel32_stub_table() };
    return &table;
}

}
