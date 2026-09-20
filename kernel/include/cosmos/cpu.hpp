#pragma once
#include <cosmos/types.hpp>

namespace cpu {

ALWAYS_INLINE void out8(u16 port, u8 val) {
    asm volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}
ALWAYS_INLINE u8 in8(u16 port) {
    u8 v; asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v;
}
ALWAYS_INLINE void out32(u16 port, u32 val) {
    asm volatile("outl %0, %1" :: "a"(val), "Nd"(port));
}
ALWAYS_INLINE u32 in32(u16 port) {
    u32 v; asm volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v;
}

ALWAYS_INLINE void io_wait() { out8(0x80, 0); }

ALWAYS_INLINE u64 rdmsr(u32 msr) {
    u32 lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (static_cast<u64>(hi) << 32) | lo;
}
ALWAYS_INLINE void wrmsr(u32 msr, u64 val) {
    u32 lo = static_cast<u32>(val);
    u32 hi = static_cast<u32>(val >> 32);
    asm volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(msr));
}

ALWAYS_INLINE void cpuid(u32 leaf, u32 subleaf, u32* a, u32* b, u32* c, u32* d) {
    asm volatile("cpuid"
                 : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                 : "a"(leaf), "c"(subleaf));
}

ALWAYS_INLINE void cli() { asm volatile("cli"); }
ALWAYS_INLINE void sti() { asm volatile("sti"); }
ALWAYS_INLINE void halt() { asm volatile("hlt"); }
NORETURN ALWAYS_INLINE void hang() { for (;;) { cli(); halt(); } }

ALWAYS_INLINE u64 read_cr3() {
    u64 v; asm volatile("mov %%cr3, %0" : "=r"(v)); return v;
}
ALWAYS_INLINE void write_cr3(u64 v) {
    asm volatile("mov %0, %%cr3" :: "r"(v) : "memory");
}

ALWAYS_INLINE void set_gs_base(u64 v) { wrmsr(0xC0000101, v); }
ALWAYS_INLINE u64  get_gs_base()      { return rdmsr(0xC0000101); }

struct Spinlock {
    volatile int locked = 0;
    void lock() {
        while (__atomic_exchange_n(&locked, 1, __ATOMIC_ACQUIRE))
            while (locked) asm volatile("pause");
    }
    void unlock() { __atomic_store_n(&locked, 0, __ATOMIC_RELEASE); }
};

}
