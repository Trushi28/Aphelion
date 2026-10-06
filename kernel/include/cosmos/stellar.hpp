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
constexpr u32 NAME_MAX_LEN = 51;

constexpr u32 FLAG_EXTENT_TABLE = 1u << 0;
constexpr u32 USER_FLAGS_MASK = 0xFFFF0000u;

struct StatInfo {
    u32 type, nlink, flags, sector_count;
    u64 size_bytes, mtime, gen;
};

enum class Status : i32 {
    Ok = 0,
    NotMounted,
    NotFormatted,
    InvalidArgument,
    InvalidName,
    NotFound,
    Exists,
    NotADirectory,
    IsADirectory,
    NotEmpty,
    NoSpace,
    NoMemory,
    Io,
    Checksum,
    Corrupt,
    Busy,
    NoSuchSnapshot,
    TooManySnapshots,
    Unsupported,
    Internal,
};
const char* status_name(Status s);

constexpr u64 READ_ERROR = INVALID_STAR;

struct IoStats { u64 reads, writes, cache_hits; };

struct LookupStats {
    u64 index_builds, index_grows, index_lookups, scan_lookups;
    u64 bloom_builds, bloom_rejects, bloom_passes, bloom_false_positives;
};

bool format(u64 total_sectors, Status* why = nullptr);
bool mount(Status* why = nullptr);
void init(u64 hhdm_offset);

u64 create_file(u64 parent, const char* name, const void* data, u64 size, Status* why = nullptr);
u64 create_constellation(u64 parent, const char* name, Status* why = nullptr);

u64 read_file(u64 star, void* buf, u64 max_size, u64 snap = LIVE, Status* why = nullptr);
u64 find(u64 dir_star, const char* name, u64 snap = LIVE, Status* why = nullptr);
u64 resolve(const char* path, u64 snap = LIVE, Status* why = nullptr);
u64 resolve_parent(const char* path, char* leaf, u64 leaf_size, u64 snap = LIVE, Status* why = nullptr);

using ListCallback = void (*)(const char* name, u64 star, u32 type, void* ctx);
void list(u64 dir_star, ListCallback cb, void* ctx, u64 snap = LIVE, Status* why = nullptr);

u64 write_file(u64 star, const void* data, u64 size, Status* why = nullptr);
u64 snapshot(Status* why = nullptr);
u32 snapshot_count();
bool link(u64 dir_star, const char* name, u64 target, Status* why = nullptr);
bool unlink(u64 dir_star, const char* name, Status* why = nullptr);
bool delete_snapshot(u64 snap, Status* why = nullptr);
u32 live_snapshot_count();
u64 gc(Status* why = nullptr);
void begin_batch();
bool end_batch(Status* why = nullptr);
u32 crc32(const void* data, u64 len);
bool verify_file(u64 star, u64 snap = LIVE, Status* why = nullptr);

IoStats io_stats();
LookupStats lookup_stats();
u64 free_space_sectors();
u64 total_sectors();
bool stat(u64 star, StatInfo* out, u64 snap = LIVE, Status* why = nullptr);
bool set_flags(u64 star, u32 flags, Status* why = nullptr);
void set_clock(u64 (*fn)());

void test_set_hash_mask(u64 mask);

}
