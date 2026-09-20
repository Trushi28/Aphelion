#pragma once
#include <cosmos/types.hpp>
#include <cosmos/pmm.hpp>

namespace constellation {

constexpr u64 PRESENT = 1ull << 0;
constexpr u64 WRITABLE = 1ull << 1;
constexpr u64 NO_CACHE = 1ull << 4;
constexpr u64 HUGE_PAGE = 1ull << 7;
constexpr u64 NO_EXECUTE = 1ull << 63;

void init(u64 hhdm_offset, u64 kernel_phys_base, u64 kernel_virt_base,
          u64 kernel_image_size, u64 max_phys_addr);

void map_2m(u64 virt_addr, u64 phys_addr, u64 flags);

void map_4k(u64 virt_addr, u64 phys_addr, u64 flags);

u64 core_pml4_phys();

}
