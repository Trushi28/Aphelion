#pragma once
#include <cosmos/types.hpp>

namespace timemath {

constexpr u64 PIT_HZ = 1193182;
constexpr u64 PM_TIMER_HZ = 3579545;
constexpr u64 FEMTOSECONDS_PER_MS = 1000000000000ull;
constexpr u32 HPET_MAX_PERIOD_FS = 100000000;

constexpr u64 hpet_ticks(u32 window_ms, u32 period_fs) {
    return period_fs ? static_cast<u64>(window_ms) * FEMTOSECONDS_PER_MS / period_fs : 0;
}

constexpr u64 pm_ticks(u32 window_ms) { return PM_TIMER_HZ * window_ms / 1000; }
constexpr u64 pit_ticks(u32 window_ms) { return PIT_HZ * window_ms / 1000; }

constexpr u64 elapsed(u64 start, u64 now, u64 mask) { return (now - start) & mask; }

constexpr u64 counter_mask(bool wide, u32 bits) { return wide ? ~0ull : (1ull << bits) - 1; }

constexpr bool within_percent(u64 a, u64 b, u32 percent) {
    u64 hi = a > b ? a : b;
    u64 lo = a > b ? b : a;
    return hi - lo <= hi * percent / 100;
}

constexpr bool plausible_hpet_period(u32 period_fs) { return period_fs != 0 && period_fs <= HPET_MAX_PERIOD_FS; }

constexpr u64 khz_from(u64 ticks, u32 window_ms) { return window_ms ? ticks / window_ms : 0; }

}
