#pragma once
#include <cosmos/types.hpp>

namespace cpu {

ALWAYS_INLINE void out8(u16 port, u8 val) {
    asm volatile("outb %0, %1" :: "a"(val), "Nd"(port) : "memory");
}
ALWAYS_INLINE u8 in8(u16 port) {
    u8 v; asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port) : "memory"); return v;
}
ALWAYS_INLINE void out32(u16 port, u32 val) {
    asm volatile("outl %0, %1" :: "a"(val), "Nd"(port) : "memory");
}
ALWAYS_INLINE u32 in32(u16 port) {
    u32 v; asm volatile("inl %1, %0" : "=a"(v) : "Nd"(port) : "memory"); return v;
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
    asm volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(msr) : "memory");
}

ALWAYS_INLINE u64 rdtsc() {
    u32 lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<u64>(hi) << 32) | lo;
}

ALWAYS_INLINE void cpuid(u32 leaf, u32 subleaf, u32* a, u32* b, u32* c, u32* d) {
    asm volatile("cpuid"
                 : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                 : "a"(leaf), "c"(subleaf));
}

ALWAYS_INLINE void cli() { asm volatile("cli" ::: "memory"); }
ALWAYS_INLINE void sti() { asm volatile("sti" ::: "memory"); }
ALWAYS_INLINE void halt() { asm volatile("hlt" ::: "memory"); }
ALWAYS_INLINE void sti_halt() { asm volatile("sti; hlt" ::: "memory"); }
ALWAYS_INLINE u64 irq_save() {
    u64 flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}
ALWAYS_INLINE void irq_restore(u64 flags) {
    if (flags & (1ull << 9)) asm volatile("sti" ::: "memory");
}
NORETURN ALWAYS_INLINE void hang() { for (;;) { cli(); halt(); } }

ALWAYS_INLINE u64 read_cr3() {
    u64 v; asm volatile("mov %%cr3, %0" : "=r"(v)); return v;
}
ALWAYS_INLINE void write_cr3(u64 v) {
    asm volatile("mov %0, %%cr3" :: "r"(v) : "memory");
}
ALWAYS_INLINE u64 read_cr0() {
    u64 v; asm volatile("mov %%cr0, %0" : "=r"(v)); return v;
}
ALWAYS_INLINE void write_cr0(u64 v) {
    asm volatile("mov %0, %%cr0" :: "r"(v) : "memory");
}
ALWAYS_INLINE u64 read_cr4() {
    u64 v; asm volatile("mov %%cr4, %0" : "=r"(v)); return v;
}
ALWAYS_INLINE void write_cr4(u64 v) {
    asm volatile("mov %0, %%cr4" :: "r"(v) : "memory");
}
ALWAYS_INLINE u64 xgetbv(u32 index) {
    u32 lo, hi;
    asm volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(index));
    return (static_cast<u64>(hi) << 32) | lo;
}
ALWAYS_INLINE void xsetbv(u32 index, u64 v) {
    asm volatile("xsetbv" :: "c"(index), "a"(static_cast<u32>(v)), "d"(static_cast<u32>(v >> 32)) : "memory");
}
ALWAYS_INLINE u64 read_u64(u64 va) { return *reinterpret_cast<volatile u64*>(va); }
ALWAYS_INLINE void clflush(const void* p) {
    asm volatile("clflush (%0)" :: "r"(p) : "memory");
}
ALWAYS_INLINE void invlpg(u64 addr) {
    asm volatile("invlpg (%0)" :: "r"(addr) : "memory");
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
