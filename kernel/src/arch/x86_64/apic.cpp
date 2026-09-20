#include <cosmos/apic.hpp>
#include <cosmos/cpu.hpp>

namespace apic {

constexpr u32 MSR_APIC_BASE   = 0x1B;
constexpr u32 MSR_X2APIC_ID   = 0x802;
constexpr u32 MSR_X2APIC_EOI  = 0x80B;
constexpr u32 MSR_X2APIC_SVR  = 0x80F;
constexpr u32 MSR_X2APIC_ICR  = 0x830;
constexpr u32 MSR_X2APIC_LVT_TIMER = 0x832;
constexpr u32 MSR_X2APIC_TIMER_INIT = 0x838;
constexpr u32 MSR_X2APIC_TIMER_DIV  = 0x83E;

constexpr u32 REG_ID          = 0x020;
constexpr u32 REG_EOI         = 0x0B0;
constexpr u32 REG_SVR         = 0x0F0;
constexpr u32 REG_ICR_LOW     = 0x300;
constexpr u32 REG_ICR_HIGH    = 0x310;
constexpr u32 REG_LVT_TIMER   = 0x320;
constexpr u32 REG_TIMER_INIT  = 0x380;
constexpr u32 REG_TIMER_DIV   = 0x3E0;

static bool g_use_x2apic = false;
static volatile u32* g_xapic_mmio = nullptr;
static u64 g_xapic_phys_base = 0;

bool x2apic_supported() {
    u32 a, b, c, d;
    cpu::cpuid(1, 0, &a, &b, &c, &d);
    return (c >> 21) & 1;
}

bool using_x2apic() { return g_use_x2apic; }
u64 xapic_mmio_phys_base() { return g_xapic_phys_base; }

u64 probe_mmio_phys_base() {
    return cpu::rdmsr(MSR_APIC_BASE) & 0xFFFFF000ull;
}

static inline u32 xapic_read(u32 off) { return g_xapic_mmio[off / 4]; }
static inline void xapic_write(u32 off, u32 val) { g_xapic_mmio[off / 4] = val; }

void start_timer_stopwatch(u8 divide_pow2) {
    static const u8 div_encoding[8] = {0xB, 0x0, 0x1, 0x2, 0x3, 0x8, 0x9, 0xA};
    u8 enc = div_encoding[divide_pow2 & 7];
    if (g_use_x2apic) {
        cpu::wrmsr(MSR_X2APIC_TIMER_DIV, enc);
        cpu::wrmsr(MSR_X2APIC_LVT_TIMER, (1u << 16));
        cpu::wrmsr(MSR_X2APIC_TIMER_INIT, 0xFFFFFFFFu);
    } else {
        xapic_write(REG_TIMER_DIV, enc);
        xapic_write(REG_LVT_TIMER, (1u << 16));
        xapic_write(REG_TIMER_INIT, 0xFFFFFFFFu);
    }
}

u32 read_timer_current() {
    constexpr u32 MSR_X2APIC_TIMER_CUR = 0x839;
    constexpr u32 REG_TIMER_CUR = 0x390;
    if (g_use_x2apic) return static_cast<u32>(cpu::rdmsr(MSR_X2APIC_TIMER_CUR));
    return xapic_read(REG_TIMER_CUR);
}

void init(u64 hhdm_offset) {

    cpu::out8(0x21, 0xFF);
    cpu::out8(0xA1, 0xFF);

    u64 base = cpu::rdmsr(MSR_APIC_BASE);
    g_xapic_phys_base = base & 0xFFFFF000ull;
    g_use_x2apic = x2apic_supported();

    if (g_use_x2apic) {
        base |= (1u << 10);
        base |= (1u << 11);
        cpu::wrmsr(MSR_APIC_BASE, base);
        cpu::wrmsr(MSR_X2APIC_SVR, 0x1FF);
    } else {
        base |= (1u << 11);
        cpu::wrmsr(MSR_APIC_BASE, base);
        g_xapic_mmio = reinterpret_cast<volatile u32*>(hhdm_offset + g_xapic_phys_base);
        xapic_write(REG_SVR, 0x1FF);
    }
}

u32 id() {
    if (g_use_x2apic) return static_cast<u32>(cpu::rdmsr(MSR_X2APIC_ID));
    return xapic_read(REG_ID) >> 24;
}

void eoi() {
    if (g_use_x2apic) cpu::wrmsr(MSR_X2APIC_EOI, 0);
    else xapic_write(REG_EOI, 0);
}

void start_timer_periodic(u32 vector, u32 initial_count, u8 divide_pow2) {

    static const u8 div_encoding[8] = {0xB, 0x0, 0x1, 0x2, 0x3, 0x8, 0x9, 0xA};
    u8 enc = div_encoding[divide_pow2 & 7];
    if (g_use_x2apic) {
        cpu::wrmsr(MSR_X2APIC_TIMER_DIV, enc);
        cpu::wrmsr(MSR_X2APIC_LVT_TIMER, vector | (1u << 17));
        cpu::wrmsr(MSR_X2APIC_TIMER_INIT, initial_count);
    } else {
        xapic_write(REG_TIMER_DIV, enc);
        xapic_write(REG_LVT_TIMER, vector | (1u << 17));
        xapic_write(REG_TIMER_INIT, initial_count);
    }
}

void send_init_ipi(u32 target_apic_id) {
    if (g_use_x2apic) {
        u64 icr = (static_cast<u64>(target_apic_id) << 32) | (0b101u << 8);
        cpu::wrmsr(MSR_X2APIC_ICR, icr);
    } else {
        xapic_write(REG_ICR_HIGH, target_apic_id << 24);
        xapic_write(REG_ICR_LOW, (0b101u << 8));
    }
}

void send_startup_ipi(u32 target_apic_id, u8 vector) {
    if (g_use_x2apic) {
        u64 icr = (static_cast<u64>(target_apic_id) << 32) | (0b110u << 8) | vector;
        cpu::wrmsr(MSR_X2APIC_ICR, icr);
    } else {
        xapic_write(REG_ICR_HIGH, target_apic_id << 24);
        xapic_write(REG_ICR_LOW, (0b110u << 8) | vector);
    }
}

}
