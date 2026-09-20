#pragma once
#include <cosmos/types.hpp>

namespace universe {

constexpr int MAX_ORDER = 18;
constexpr u64 PAGE_SIZE = 4096;

struct MemmapEntry { u64 base, length, type; };

void init(const MemmapEntry* entries, u64 count, u64 hhdm_offset);

void reclaim_bootloader_regions(const MemmapEntry* entries, u64 count);

u64 alloc(int order);
void free(u64 phys_addr, int order);

u64 free_bytes();
u64 total_bytes();
u32 galaxy_count();

}
