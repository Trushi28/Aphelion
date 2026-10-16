#pragma once
#include <cosmos/types.hpp>

namespace fpu {

constexpr u32 MAX_CORES = 64;

#ifdef APHELION_FPU
constexpr bool ENABLED = true;
#else
constexpr bool ENABLED = false;
#endif

void init_core();
void report();

u32 cores_seen();
bool selftest();

}
