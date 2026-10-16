#pragma once
#include <cosmos/types.hpp>
#include <stdlib.h>

namespace cpu {

inline void invlpg(u64) {}
inline void write_cr3(u64) {}
inline u64 irq_save() { return 0; }
inline void irq_restore(u64) {}
[[noreturn]] inline void hang() { abort(); }

struct Spinlock {
    volatile int locked = 0;
    void lock() { if (__atomic_exchange_n(&locked, 1, __ATOMIC_ACQUIRE)) abort(); }
    void unlock() { __atomic_store_n(&locked, 0, __ATOMIC_RELEASE); }
};

}
