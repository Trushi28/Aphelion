#include <cosmos/timesrc.hpp>
#include <cosmos/timemath.hpp>
#include <cosmos/apic.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/vmm.hpp>
#include <cosmos/serial.hpp>

namespace timesrc {

namespace {

constexpr u64 HPET_CAPS = 0x00;
constexpr u64 HPET_CONFIG = 0x10;
constexpr u64 HPET_COUNTER = 0xF0;
constexpr u64 HPET_CAP_WIDE = 1ull << 13;
constexpr u64 HPET_CFG_ENABLE = 1;
constexpr u16 PIT_CH0 = 0x40;
constexpr u16 PIT_CMD = 0x43;
constexpr u32 MAX_WINDOW_MS = 40;
constexpr u64 FIRST_GUARD_TSC = 4000000000ull;
constexpr u64 MIN_GUARD_TSC = 100000000ull;
constexpr u32 GUARD_FACTOR = 50;
constexpr u32 ASSUMED_APIC_TICKS_PER_10MS = 62500;
constexpr u32 AGREEMENT_PERCENT = 25;

struct Hpet {
    volatile u8* regs = nullptr;
    u32 period_fs = 0;
    bool wide = false;
    bool usable = false;
};

Hpet g_hpet;
bool g_pm_present = false;
u16 g_pm_port = 0;
bool g_pm_32bit = false;
Source g_order[SOURCE_COUNT];
u32 g_order_count = 0;
Calibration g_last;
u64 g_tsc_khz = 0;

struct Probe {
    u64 (*read)();
    u64 mask;
    u64 target;
};

u64 hpet_read(u64 offset) { return *reinterpret_cast<volatile u64*>(g_hpet.regs + offset); }
void hpet_write(u64 offset, u64 value) { *reinterpret_cast<volatile u64*>(g_hpet.regs + offset) = value; }

u64 hpet_counter() { return hpet_read(HPET_COUNTER); }
u64 pm_counter() { return cpu::in32(g_pm_port); }

u64 pit_counter() {
    cpu::out8(PIT_CMD, 0x00);
    u8 lo = cpu::in8(PIT_CH0);
    u8 hi = cpu::in8(PIT_CH0);
    u16 down = static_cast<u16>(lo | (hi << 8));
    return static_cast<u16>(0x10000u - down);
}

void pit_program() {
    cpu::out8(PIT_CMD, 0x34);
    cpu::out8(PIT_CH0, 0xFF);
    cpu::out8(PIT_CH0, 0xFF);
}

bool available(Source s) {
    switch (s) {
        case Source::Hpet: return g_hpet.usable;
        case Source::PmTimer: return g_pm_present;
        case Source::Pit: return true;
        default: return false;
    }
}

void build_order(bootcfg::TimerPref pref) {
    Source natural[SOURCE_COUNT] = { Source::Hpet, Source::PmTimer, Source::Pit };
    Source first = Source::None;
    switch (pref) {
        case bootcfg::TimerPref::Hpet: first = Source::Hpet; break;
        case bootcfg::TimerPref::Pm: first = Source::PmTimer; break;
        case bootcfg::TimerPref::Pit: first = Source::Pit; break;
        default: break;
    }
    g_order_count = 0;
    if (first != Source::None && available(first)) g_order[g_order_count++] = first;
    else if (first != Source::None)
        serial::printf("[timer] %s requested but not present, using the automatic order\n", name(first));
    for (Source s : natural)
        if (s != first && available(s)) g_order[g_order_count++] = s;
}

Measurement measure(Source src, u32 window_ms, u8 divide_pow2, u64 guard_tsc) {
    Measurement m;
    m.source = src;
    Probe p{};
    switch (src) {
        case Source::Hpet:
            if (!g_hpet.usable) return m;
            p = { &hpet_counter, timemath::counter_mask(g_hpet.wide, 32), timemath::hpet_ticks(window_ms, g_hpet.period_fs) };
            break;
        case Source::PmTimer:
            if (!g_pm_present) return m;
            p = { &pm_counter, timemath::counter_mask(g_pm_32bit, 24), timemath::pm_ticks(window_ms) };
            break;
        case Source::Pit:
            pit_program();
            p = { &pit_counter, 0xFFFF, timemath::pit_ticks(window_ms) };
            break;
        default:
            return m;
    }
    if (p.target == 0 || p.target >= p.mask) return m;

    apic::start_timer_stopwatch(divide_pow2);
    u64 t0 = cpu::rdtsc();
    u64 start = p.read() & p.mask;
    u64 spent = 0;
    bool done = false;
    for (;;) {
        u64 now = p.read() & p.mask;
        spent = timemath::elapsed(start, now, p.mask);
        if (spent >= p.target) { done = true; break; }
        if (cpu::rdtsc() - t0 > guard_tsc) break;
        asm volatile("pause" ::: "memory");
    }
    u32 apic_left = apic::read_timer_current();
    u64 t1 = cpu::rdtsc();
    if (!done || apic_left == 0) return m;

    u64 apic_delta = 0xFFFFFFFFull - apic_left;
    u64 scaled = apic_delta * p.target / spent;
    if (scaled == 0 || scaled > 0xFFFFFFFFull) return m;
    m.apic_ticks = static_cast<u32>(scaled);
    m.tsc_ticks = (t1 - t0) * p.target / spent;
    m.ok = true;
    return m;
}

}

void init(const acpi::Info& info, u64 hhdm_offset, bootcfg::TimerPref pref) {
    g_hpet = Hpet{};
    g_pm_present = info.pm_timer_found;
    g_pm_port = info.pm_timer_port;
    g_pm_32bit = info.pm_timer_32bit;

    if (info.hpet_found) {
        u64 page = info.hpet_base & ~0xFFFull;
        u64 offset = info.hpet_base & 0xFFF;
        constexpr u64 flags = constellation::WRITABLE | constellation::NO_CACHE | constellation::NO_EXECUTE;
        if (offset + 0x100 <= 0x1000 && constellation::map_4k(hhdm_offset + page, page, flags)) {
            g_hpet.regs = reinterpret_cast<volatile u8*>(hhdm_offset + page + offset);
            u64 caps = hpet_read(HPET_CAPS);
            u32 period = static_cast<u32>(caps >> 32);
            if (timemath::plausible_hpet_period(period)) {
                g_hpet.period_fs = period;
                g_hpet.wide = (caps & HPET_CAP_WIDE) != 0;
                u64 config = hpet_read(HPET_CONFIG);
                if (!(config & HPET_CFG_ENABLE)) hpet_write(HPET_CONFIG, config | HPET_CFG_ENABLE);
                g_hpet.usable = true;
                serial::printf("[timer] HPET at %p: period %u fs, %s counter\n", info.hpet_base, period,
                                g_hpet.wide ? "64-bit" : "32-bit");
            } else {
                serial::printf("[timer] HPET at %p reports an implausible period (%u fs), ignoring it\n", info.hpet_base, period);
            }
        } else {
            serial::writeln("[timer] could not map the HPET registers, ignoring it");
        }
    }
    if (g_pm_present)
        serial::printf("[timer] ACPI PM timer at port %lx, %s counter\n", static_cast<u64>(g_pm_port), g_pm_32bit ? "32-bit" : "24-bit");
    build_order(pref);
}

Calibration calibrate(u32 window_ms, u8 apic_divide_pow2, bool cross_check) {
    if (window_ms == 0) window_ms = 1;
    if (window_ms > MAX_WINDOW_MS) window_ms = MAX_WINDOW_MS;
    Calibration c;
    c.window_ms = window_ms;
    u64 guard = FIRST_GUARD_TSC;
    for (u32 i = 0; i < g_order_count && c.measured < SOURCE_COUNT; ++i) {
        Measurement m = measure(g_order[i], window_ms, apic_divide_pow2, guard);
        c.all[c.measured++] = m;
        if (m.ok) {
            serial::printf("[timer] %s: %u APIC timer ticks per %u ms, TSC %lu kHz\n", name(m.source), m.apic_ticks,
                            window_ms, timemath::khz_from(m.tsc_ticks, window_ms));
        } else {
            serial::printf("[timer] %s: no usable reading\n", name(m.source));
        }
        if (m.ok && c.chosen == Source::None) {
            c.chosen = m.source;
            c.apic_ticks = m.apic_ticks;
            c.tsc_khz = timemath::khz_from(m.tsc_ticks, window_ms);
            u64 next = c.tsc_khz * window_ms * GUARD_FACTOR;
            guard = next < MIN_GUARD_TSC ? MIN_GUARD_TSC : next;
            if (!cross_check) break;
        }
    }
    if (c.chosen == Source::None) {
        c.assumed = true;
        c.apic_ticks = ASSUMED_APIC_TICKS_PER_10MS * window_ms / 10;
        serial::printf("[timer] WARNING: no time source gave a reading, assuming %u APIC ticks per %u ms; uptime will be wrong\n",
                        c.apic_ticks, window_ms);
    } else {
        serial::printf("[timer] calibrated with %s: %u APIC ticks per %u ms\n", name(c.chosen), c.apic_ticks, window_ms);
    }
    g_last = c;
    g_tsc_khz = c.tsc_khz;
    return c;
}

const Calibration& last() { return g_last; }

bool agrees(const Calibration& c, u32 percent) {
    if (c.chosen == Source::None) return false;
    for (u32 i = 0; i < c.measured; ++i)
        if (c.all[i].ok && !timemath::within_percent(c.all[i].apic_ticks, c.apic_ticks, percent)) return false;
    return true;
}

bool selftest() { return !g_last.assumed && g_last.chosen != Source::None && agrees(g_last, AGREEMENT_PERCENT); }

u64 tsc_khz() { return g_tsc_khz; }

const char* name(Source s) {
    switch (s) {
        case Source::Hpet: return "HPET";
        case Source::PmTimer: return "ACPI PM timer";
        case Source::Pit: return "PIT";
        default: return "none";
    }
}

}
