#pragma once
#include <cosmos/types.hpp>

namespace tlb {

constexpr u32 MAX_CORES = 64;
constexpr u64 ALL_ADDRESSES = ~0ull;

void init_core();
void poll();

u32 core_index();
u64 online_mask();
u32 online_count();

void flush(u64 core_mask, u64 addr, u64 bytes);

bool selftest_probe(u64 addr, u64* out, u32 slots);

}
