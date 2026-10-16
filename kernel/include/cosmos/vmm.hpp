#pragma once
#include <cosmos/types.hpp>
#include <cosmos/pmm.hpp>

namespace constellation {

constexpr u64 PRESENT = 1ull << 0;
constexpr u64 WRITABLE = 1ull << 1;
constexpr u64 WRITE_THROUGH = 1ull << 3;
constexpr u64 WRITE_COMBINING = WRITE_THROUGH;
constexpr u64 NO_CACHE = 1ull << 4;
constexpr u64 HUGE_PAGE = 1ull << 7;
constexpr u64 PAT_4K = 1ull << 7;
constexpr u64 PAT_2M = 1ull << 12;
constexpr u64 NO_EXECUTE = 1ull << 63;

constexpr u64 STACK_REGION_BASE = 0xFFFFD00000000000ull;
constexpr u64 STACK_SLOT_BYTES = 0x10000;
constexpr u64 STACK_GUARD_BYTES = 0x1000;
constexpr u64 STACK_BYTES = 0x8000;
constexpr u32 STACK_SLOTS = 512;

constexpr u64 stack_slot_base(u32 slot) { return STACK_REGION_BASE + static_cast<u64>(slot) * STACK_SLOT_BYTES; }
constexpr u64 stack_slot_bottom(u32 slot) { return stack_slot_base(slot) + STACK_GUARD_BYTES; }
constexpr u64 stack_slot_top(u32 slot) { return stack_slot_bottom(slot) + STACK_BYTES; }

bool in_stack_guard(u64 addr);

struct KernelImage {
    u64 phys_base, virt_base;
    u64 requests_start, requests_end;
    u64 text_start, text_end;
    u64 rodata_start, rodata_end;
    u64 data_start, data_end;
};

void init(u64 hhdm_offset, const universe::MemmapEntry* entries, u64 count, const KernelImage& image);

bool map_2m(u64 virt_addr, u64 phys_addr, u64 flags);

bool map_4k(u64 virt_addr, u64 phys_addr, u64 flags);

bool map_range(u64 virt_addr, u64 phys_addr, u64 bytes, u64 flags);

struct Mapping {
    u64 phys;
    u64 size;
    u64 flags;
    bool pat;
};

bool translate(u64 virt_addr, Mapping* out);

void seal();

bool selftest();

u64 core_pml4_phys();

}
