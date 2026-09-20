#include <cosmos/pit.hpp>
#include <cosmos/apic.hpp>
#include <cosmos/cpu.hpp>

namespace pit {

constexpr u32 PIT_HZ = 1193182;
constexpr u16 PORT_CH0 = 0x40;
constexpr u16 PORT_CMD = 0x43;

static u16 latch_read_ch0() {
    cpu::out8(PORT_CMD, 0x00);
    u8 lo = cpu::in8(PORT_CH0);
    u8 hi = cpu::in8(PORT_CH0);
    return static_cast<u16>(lo | (hi << 8));
}

u32 calibrate_apic_ticks(u32 window_ms, u8 apic_divide_pow2) {

    cpu::out8(PORT_CMD, 0x34);
    cpu::out8(PORT_CH0, 0xFF);
    cpu::out8(PORT_CH0, 0xFF);

    u32 target_pit_ticks = (PIT_HZ / 1000) * window_ms;

    apic::start_timer_stopwatch(apic_divide_pow2);
    u16 pit_start = latch_read_ch0();

    u32 elapsed = 0;
    u16 last = pit_start;
    while (elapsed < target_pit_ticks) {
        u16 now = latch_read_ch0();
        u16 delta = static_cast<u16>(last - now);
        elapsed += delta;
        last = now;
    }

    u32 apic_now = apic::read_timer_current();
    return 0xFFFFFFFFu - apic_now;
}

}
