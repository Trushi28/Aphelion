#pragma once
#include <cosmos/types.hpp>

namespace fpu {

constexpr u32 MAX_CORES = 64;
constexpr u32 STATE_MAX_BYTES = 1024;

#ifdef APHELION_FPU
constexpr bool ENABLED = true;
#else
constexpr bool ENABLED = false;
#endif

void init_core();
void report();

bool usable();
bool avx_state();
u32 state_bytes();
void init_state(void* area);
void save(void* area);
void restore(const void* area);

struct Modes { u16 control_word; u32 mxcsr; };
Modes default_modes();
void set_modes(const Modes& m);
Modes get_modes();
void load_registers(const void* buf);
void store_registers(void* buf);
u32 register_bytes();

u32 cores_seen();
bool selftest();

}
