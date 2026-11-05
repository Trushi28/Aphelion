#pragma once
#include <cosmos/types.hpp>
#include <cosmos/lens_types.hpp>
#include <cosmos/userabi.hpp>
#include <cosmos/handleabi.hpp>

namespace fileabi {

constexpr i64 ERR_NOTFOUND = -15;
constexpr i64 ERR_WRONGTYPE = -16;

constexpr u32 MAX_PATH = 255;
constexpr u32 MAX_IO = 1024;
constexpr u32 NAME_CAP = 256;
constexpr u64 MAX_PIPE_BYTES = 65536;

constexpr u32 CREATE_DIRECTORY = 1;
constexpr u32 CREATE_FLAGS_MASK = CREATE_DIRECTORY;

constexpr u32 KIND_FILE = 0;
constexpr u32 KIND_DIRECTORY = 1;
constexpr u32 KIND_STREAM = 2;

constexpr u32 STAT_READ_ONLY = 1;
constexpr u32 STAT_DYNAMIC = 2;

constexpr i64 DIR_END = 1;

struct Stat {
    u64 size;
    u64 mtime_ms;
    u32 kind;
    u32 links;
    u32 flags;
    u32 reserved;
};

struct DirEntry {
    u64 size;
    u64 mtime_ms;
    u32 kind;
    u32 name_len;
    char name[NAME_CAP];
};

static_assert(sizeof(Stat) == 32, "the stat record is part of the user ABI");
static_assert(sizeof(DirEntry) == 24 + NAME_CAP, "the directory entry record is part of the user ABI");

constexpr lens::Rights PROCESS_RIGHTS = lens::Right::Read | lens::Right::Write | lens::Right::Delegate;
constexpr lens::Rights LAUNCH_RIGHTS = lens::Right::Read | lens::Right::Execute;

constexpr bool path_len_ok(u64 len) { return len != 0 && len <= MAX_PATH; }

constexpr bool rights_ok(u64 raw) { return (raw & ~static_cast<u64>(lens::RIGHTS_ALL_BITS)) == 0; }

constexpr u64 clamp_io(u64 len) { return len < MAX_IO ? len : MAX_IO; }

constexpr bool path_text_ok(const char* s, u64 len) {
    if (!path_len_ok(len)) return false;
    for (u64 i = 0; i < len; ++i) {
        if (s[i] == 0) return false;
    }
    return true;
}

constexpr i64 error_of(lens::Status s) {
    using lens::Status;
    switch (s) {
        case Status::NotFound: return ERR_NOTFOUND;
        case Status::Exists: return handleabi::ERR_EXISTS;
        case Status::NotADirectory:
        case Status::IsADirectory:
        case Status::NotAFile: return ERR_WRONGTYPE;
        default: return handleabi::error_of(s);
    }
}

constexpr u32 stat_flags(bool read_only, bool dynamic) {
    return (read_only ? STAT_READ_ONLY : 0u) | (dynamic ? STAT_DYNAMIC : 0u);
}

}
