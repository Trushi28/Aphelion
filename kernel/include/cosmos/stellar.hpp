#pragma once
#include <cosmos/types.hpp>

namespace stellar {

constexpr u32 TYPE_FREE = 0;
constexpr u32 TYPE_FILE = 1;
constexpr u32 TYPE_CONSTELLATION = 2;

constexpr u64 ROOT_STAR = 0;
constexpr u64 INVALID_STAR = 0xFFFFFFFFFFFFFFFFull;

bool format(u64 total_sectors);
bool mount();
void init(u64 hhdm_offset);

u64 create_file(u64 parent, const char* name, const void* data, u64 size);
u64 create_constellation(u64 parent, const char* name);

u64 read_file(u64 star, void* buf, u64 max_size);
u64 find(u64 dir_star, const char* name);

using ListCallback = void (*)(const char* name, u64 star, u32 type, void* ctx);
void list(u64 dir_star, ListCallback cb, void* ctx);

}
