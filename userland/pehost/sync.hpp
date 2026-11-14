#pragma once
#include "host.hpp"

namespace pehost {

constexpr u32 STATUS_POSSIBLE_DEADLOCK = 0xC0000194u;

namespace detail {

inline u32 sync_text_length(const char* s) {
    u32 n = 0;
    while (s[n]) ++n;
    return n;
}

inline void say_error(const char* text) { g_runtime->write_console(2, text, sync_text_length(text)); }

inline void hex32(char* out, u32 v) {
    const char* digits = "0123456789ABCDEF";
    for (u32 i = 0; i < 8; ++i) out[i] = digits[(v >> (28 - 4 * i)) & 15];
    out[8] = 0;
}

[[noreturn]] inline void stop(const char* what, u32 code) {
    char tail[16];
    hex32(tail, code);
    say_error("\n[pehost] ");
    say_error(what);
    say_error(" (status 0x");
    say_error(tail);
    say_error(")\n");
    g_runtime->exit_process(code);
    for (;;) asm volatile("ud2");
}

}

namespace sync {

constexpr u64 FOREVER = ~0ull;
constexpr u32 MAX_SLOTS = 8;
constexpr u32 WORKER_LIMIT = MAX_SLOTS - 1;
constexpr u32 MODULE_TLS = modules::TLS_LIMIT;

struct ModuleBlock {
    u8* block;
    u64 bytes;
};

struct Slot {
    bool used;
    bool running;
    Teb* teb;
    u64* tls_array;
    u64* fls;
    u8* support;
    u64 support_bytes;
    u8* stack;
    u64 stack_bytes;
    volatile u32 job;
    volatile u32 private_word;
    i32 object;
    const volatile u32* waiting_word;
    u32 waiting_expected;
    volatile u32 waiting_forever;
    volatile u32 waiting_serial;
    u64 context[8];
    ModuleBlock module_tls[MODULE_TLS];
};

constexpr u32 MAX_OBJECTS = 96;
constexpr u32 MAX_HANDLES = 128;
constexpr u32 OBJECT_NAME = 48;

constexpr u8 OBJ_NONE = 0;
constexpr u8 OBJ_EVENT = 1;
constexpr u8 OBJ_MUTEX = 2;
constexpr u8 OBJ_SEMAPHORE = 3;
constexpr u8 OBJ_THREAD = 4;

struct Object {
    u8 kind;
    bool manual;
    bool abandoned;
    bool signaled;
    bool finished;
    bool suspended;
    bool auto_close;
    u32 refs;
    i32 count;
    i32 maximum;
    u32 recursion;
    u32 id;
    u32 exit_code;
    i32 slot;
    u64 owner;
    void* start;
    void* parameter;
    volatile u32 gate;
    char name[OBJECT_NAME];
};

struct ObjectTable {
    Object items[MAX_OBJECTS];
    u32 handles[MAX_HANDLES];
    volatile u32 epoch;
};

struct Lock {
    volatile u32 word;
    volatile u64 owner;
    u32 depth;
};

struct State {
    Slot slots[MAX_SLOTS];
    ObjectTable objects;
    volatile u32 multi;
    bool main_gone;
    u32 live;
    u32 next_id;
    Lock host;
    Lock loader;
    Lock stdio;
    Lock crt;
};

inline State g_sync;

inline Lock* const* all_locks() {
    static Lock* const list[] = { &g_sync.host, &g_sync.loader, &g_sync.stdio, &g_sync.crt };
    return list;
}

constexpr u32 LOCK_COUNT = 4;

inline u32 load(const volatile u32* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
inline void store(volatile u32* p, u32 v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
inline u32 add(volatile u32* p, u32 v) { return __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST); }
inline u32 exchange(volatile u32* p, u32 v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
inline bool cas(volatile u32* p, u32 expected, u32 desired) {
    return __atomic_compare_exchange_n(p, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
inline u32 cas_value(volatile u32* p, u32 expected, u32 desired) {
    __atomic_compare_exchange_n(p, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}

inline bool available() { return g_runtime && g_runtime->futex_wait && g_runtime->futex_wake; }

inline Teb* teb_now() {
    Teb* t;
    asm volatile("movq %%gs:0x30, %0" : "=r"(t));
    return t;
}

inline Slot& current() {
    if (!g_sync.multi) return g_sync.slots[0];
    u64 index = teb_now()->host_slot;
    return g_sync.slots[index < MAX_SLOTS ? index : 0];
}

inline u64 self_key() { return reinterpret_cast<u64>(teb_now()); }

inline void wake(volatile u32* word) {
    if (available()) g_runtime->futex_wake(word);
}

inline bool counted(u32 index) {
    if (index == 0) return !g_sync.main_gone;
    return g_sync.slots[index].used && g_sync.slots[index].running;
}

inline bool stuck(u32 index) {
    const Slot& s = g_sync.slots[index];
    u32 before = load(&s.waiting_serial);
    if (before & 1) return false;
    if (!load(&s.waiting_forever)) return false;
    const volatile u32* word = s.waiting_word;
    u32 expected = s.waiting_expected;
    if (load(&s.waiting_serial) != before) return false;
    return word != nullptr && load(word) == expected;
}

inline bool all_stuck() {
    u32 seen = 0;
    for (u32 i = 0; i < MAX_SLOTS; ++i) {
        if (!counted(i)) continue;
        ++seen;
        if (!stuck(i)) return false;
    }
    return seen != 0;
}

inline void say_deadlock() { detail::stop("a wait that no other thread can end", STATUS_POSSIBLE_DEADLOCK); }

constexpr u32 CONFIRM_ROUNDS = 3;
constexpr u64 CONFIRM_MS = 25;

enum class Verdict { woken, keep_waiting, deadlock };

inline Verdict confirm_stuck(volatile u32* word, u32 expected) {
    for (u32 round = 0; round < CONFIRM_ROUNDS; ++round) {
        if (g_runtime->futex_wait(word, expected, CONFIRM_MS) != 1) return Verdict::woken;
        if (!all_stuck()) return Verdict::keep_waiting;
    }
    return Verdict::deadlock;
}

inline void confirm_or_stop_leaving(Slot& s) {
    if (!available() || !all_stuck()) return;
    u32 current_value = load(&s.private_word);
    for (u32 round = 0; round < CONFIRM_ROUNDS; ++round) {
        g_runtime->futex_wait(&s.private_word, current_value, CONFIRM_MS);
        if (!all_stuck()) return;
    }
    say_deadlock();
}

inline i32 block_without_futex(u64 timeout) {
    if (timeout == FOREVER) say_deadlock();
    if (g_runtime->sleep_ms && timeout) g_runtime->sleep_ms(timeout > 0xFFFFFFFEull ? 0xFFFFFFFEu : static_cast<u32>(timeout));
    return 1;
}

inline i32 block(volatile u32* word, u32 expected, u64 timeout) {
    if (!available()) return block_without_futex(timeout);
    Slot& s = current();
    bool forever = timeout == FOREVER;
    if (forever) {
        add(&s.waiting_serial, 1);
        s.waiting_word = word;
        s.waiting_expected = expected;
        add(&s.waiting_serial, 1);
        store(&s.waiting_forever, 1);
        if (all_stuck()) {
            Verdict verdict = confirm_stuck(word, expected);
            if (verdict == Verdict::deadlock) say_deadlock();
            if (verdict == Verdict::woken) {
                store(&s.waiting_forever, 0);
                return 0;
            }
        }
    }
    i32 r = g_runtime->futex_wait(word, expected, timeout);
    if (forever) store(&s.waiting_forever, 0);
    return r == 1 ? 1 : 0;
}

inline bool monotonic_ms(u64* out) {
    if (!g_runtime || !g_runtime->clock) return false;
    Clock c{};
    if (!g_runtime->clock(&c) || c.frequency == 0) return false;
    *out = c.counter / c.frequency * 1000 + c.counter % c.frequency * 1000 / c.frequency;
    return true;
}

struct Deadline {
    u64 total;
    u64 end;
    bool clocked;

    explicit Deadline(u64 milliseconds) : total(milliseconds), end(0), clocked(false) {
        if (milliseconds == FOREVER) return;
        u64 now = 0;
        if (monotonic_ms(&now)) {
            clocked = true;
            end = now + milliseconds;
        }
    }

    bool forever() const { return total == FOREVER; }

    u64 remaining() const {
        if (total == FOREVER || !clocked) return total;
        u64 now = 0;
        if (!monotonic_ms(&now) || now >= end) return 0;
        return end - now;
    }

    bool expired() const { return clocked && total != FOREVER && remaining() == 0; }
};

inline void acquire(Lock& l) {
    if (!g_sync.multi) {
        ++l.depth;
        return;
    }
    u64 me = self_key();
    if (l.owner == me) {
        ++l.depth;
        return;
    }
    u32 c = cas_value(&l.word, 0, 1);
    if (c != 0) {
        if (c != 2) c = exchange(&l.word, 2);
        while (c != 0) {
            block(&l.word, 2, FOREVER);
            c = exchange(&l.word, 2);
        }
    }
    l.owner = me;
    l.depth = 1;
}

inline void release(Lock& l) {
    if (l.depth == 0) return;
    if (!g_sync.multi) {
        --l.depth;
        return;
    }
    if (--l.depth) return;
    l.owner = 0;
    if (exchange(&l.word, 0) == 2) wake(&l.word);
}

struct Guard {
    Lock& lock;
    explicit Guard(Lock& l) : lock(l) { acquire(lock); }
    ~Guard() { release(lock); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

inline void host_lock() { acquire(g_sync.host); }
inline void host_unlock() { release(g_sync.host); }

struct Host : Guard {
    Host() : Guard(g_sync.host) {}
};

struct Loader : Guard {
    Loader() : Guard(g_sync.loader) {}
};

struct Stdio : Guard {
    Stdio() : Guard(g_sync.stdio) {}
};

inline void enter_multi() {
    if (g_sync.multi) return;
    u64 me = self_key();
    Lock* const* list = all_locks();
    for (u32 i = 0; i < LOCK_COUNT; ++i) {
        Lock& l = *list[i];
        if (l.depth != 0) {
            l.owner = me;
            l.word = 1;
        }
    }
    store(&g_sync.multi, 1);
}

inline void reset() {
    u8* raw = reinterpret_cast<u8*>(&g_sync);
    for (usize i = 0; i < sizeof(g_sync); ++i) raw[i] = 0;
    g_sync.slots[0].used = true;
    g_sync.slots[0].running = true;
    g_sync.live = 1;
}

}

}
