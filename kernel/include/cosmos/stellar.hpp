#pragma once
#include <cosmos/types.hpp>

namespace stellar {

constexpr u32 TYPE_FREE = 0;
constexpr u32 TYPE_FILE = 1;
constexpr u32 TYPE_CONSTELLATION = 2;

constexpr u32 ROOT_STAR = 0;
constexpr u32 INVALID_STAR = 0xFFFFFFFFu;

bool format(u64 total_sectors);
bool mount();
void init(u64 hhdm_offset);

u32 create_file(u32 parent, const char* name, const void* data, u64 size);
u32 create_constellation(u32 parent, const char* name);

u64 read_file(u32 star, void* buf, u64 max_size);
u32 find(u32 dir_star, const char* name);

using ListCallback = void (*)(const char* name, u32 star, u32 type, void* ctx);
void list(u32 dir_star, ListCallback cb, void* ctx);

}
