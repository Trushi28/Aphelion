#pragma once
#include <cosmos/types.hpp>

namespace stellar {

constexpr u32 TYPE_FREE = 0;
constexpr u32 TYPE_FILE = 1;
constexpr u32 TYPE_CONSTELLATION = 2;

constexpr u64 ROOT_STAR = 0;
constexpr u64 INVALID_STAR = 0xFFFFFFFFFFFFFFFFull;

constexpr u64 LIVE = 0;
constexpr u32 SNAPSHOT_MAX = 24;

struct IoStats { u64 reads, writes, cache_hits; };

bool format(u64 total_sectors);
bool mount();
void init(u64 hhdm_offset);

u64 create_file(u64 parent, const char* name, const void* data, u64 size);
u64 create_constellation(u64 parent, const char* name);

u64 read_file(u64 star, void* buf, u64 max_size, u64 snap = LIVE);
u64 find(u64 dir_star, const char* name, u64 snap = LIVE);

using ListCallback = void (*)(const char* name, u64 star, u32 type, void* ctx);
void list(u64 dir_star, ListCallback cb, void* ctx, u64 snap = LIVE);

u64 write_file(u64 star, const void* data, u64 size);
u64 snapshot();
u32 snapshot_count();
bool link(u64 dir_star, const char* name, u64 target);
bool unlink(u64 dir_star, const char* name);
bool delete_snapshot(u64 snap);
u32 live_snapshot_count();
u64 gc();
bool verify_file(u64 star, u64 snap = LIVE);

IoStats io_stats();
u64 free_space_sectors();

}
