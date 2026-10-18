#pragma once
#include <cosmos/types.hpp>

namespace kheap {

constexpr usize MAX_SMALL = 2048;
constexpr usize GUARANTEED_ALIGNMENT = 16;
constexpr u32 CLASS_COUNT = 14;

struct Stats {
    u64 live_objects;
    u64 live_bytes;
    u64 slab_pages;
    u64 large_blocks;
    u64 large_pages;
    u64 alloc_calls;
    u64 free_calls;
    u64 failed_allocs;
};

void init();
bool ready();

void* alloc(usize bytes);
void* alloc_zeroed(usize bytes);
void free(void* p);
usize usable_size(const void* p);

Stats stats();
bool check();
bool selftest();

}
