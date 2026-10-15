#pragma once
#include <cosmos/types.hpp>

namespace universe {

constexpr int MAX_ORDER = 18;
constexpr u64 PAGE_SIZE = 4096;
constexpr u64 DMA32_LIMIT = 1ull << 32;

struct MemmapEntry { u64 base, length, type; };

void init(const MemmapEntry* entries, u64 count, u64 hhdm_offset);
void reclaim_bootloader_regions(const MemmapEntry* entries, u64 count);

u64 alloc(int order);
u64 alloc_dma32(int order);
u64 alloc_zeroed(int order, bool dma32 = false);
void free(u64 phys_addr, int order);
void* to_virt(u64 phys);

u64 free_bytes();
u64 total_bytes();
u32 galaxy_count();

bool selftest();

}