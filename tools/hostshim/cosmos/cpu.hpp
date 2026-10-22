#pragma once
#include <cosmos/types.hpp>
#include <stdlib.h>
#include <sched.h>

namespace cpu {

inline u64 rdtsc() { return __builtin_ia32_rdtsc(); }
inline void invlpg(u64) {}
inline u64 (*g_read_u64)(u64) = nullptr;
inline u64 read_u64(u64 va) { return g_read_u64(va); }
inline void clflush(const void*) {}
inline u64 g_cr3 = 0;
inline void write_cr3(u64 v) { g_cr3 = v; }
inline u64 read_cr3() { return g_cr3; }
inline u64 irq_save() { return 0; }
inline void irq_restore(u64) {}
[[noreturn]] inline void hang() { abort(); }

struct Spinlock {
    volatile int locked = 0;
    void lock() {
        while (__atomic_exchange_n(&locked, 1, __ATOMIC_ACQUIRE))
            while (__atomic_load_n(&locked, __ATOMIC_RELAXED)) sched_yield();
    }
    void unlock() { __atomic_store_n(&locked, 0, __ATOMIC_RELEASE); }
};

}
