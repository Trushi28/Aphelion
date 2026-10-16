#pragma once
#include <cosmos/types.hpp>

namespace percore {

constexpr u32 MAX_CORES = 64;
constexpr u64 PAT_VALUE = 0x0007040600070106ull;

void early_init();

bool nx_enabled();
bool wc_enabled();

u32 cores_seen();
bool selftest();

}
