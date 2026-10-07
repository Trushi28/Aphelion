#pragma once
#include <cosmos/types.hpp>

namespace clock {

constexpr u32 TICK_MS = 10;

void init(u32 bsp_apic_id);
void sync();
void tick(u32 apic_id);

bool wall_valid();
u64 now_ms();
u64 uptime_ms();
u64 boot_wall_ms();

}
