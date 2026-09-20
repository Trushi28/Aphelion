#pragma once
#include <cosmos/types.hpp>

namespace pit {

u32 calibrate_apic_ticks(u32 window_ms, u8 apic_divide_pow2);

}
