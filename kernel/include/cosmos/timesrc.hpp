#pragma once
#include <cosmos/types.hpp>
#include <cosmos/acpi.hpp>
#include <cosmos/bootcfg.hpp>

namespace timesrc {

enum class Source : u8 { None, Hpet, PmTimer, Pit };

constexpr u32 SOURCE_COUNT = 3;

struct Measurement {
    Source source = Source::None;
    bool ok = false;
    u32 apic_ticks = 0;
    u64 tsc_ticks = 0;
};

struct Calibration {
    Source chosen = Source::None;
    bool assumed = false;
    u32 window_ms = 0;
    u32 apic_ticks = 0;
    u64 tsc_khz = 0;
    u32 measured = 0;
    Measurement all[SOURCE_COUNT];
};

void init(const acpi::Info& info, u64 hhdm_offset, bootcfg::TimerPref pref);
Calibration calibrate(u32 window_ms, u8 apic_divide_pow2, bool cross_check);

const Calibration& last();
bool agrees(const Calibration& c, u32 percent);
bool selftest();
u64 tsc_khz();
const char* name(Source s);

}
