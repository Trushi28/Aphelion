#pragma once
#include <cosmos/types.hpp>
#include <cosmos/lens_types.hpp>
#include <cosmos/userabi.hpp>

namespace handleabi {

constexpr i64 ERR_AGAIN = -5;
constexpr i64 ERR_CLOSED = -6;
constexpr i64 ERR_DENIED = -7;
constexpr i64 ERR_BADHANDLE = -8;
constexpr i64 ERR_TOOBIG = -9;
constexpr i64 ERR_TIMEDOUT = -10;
constexpr i64 ERR_CANCELLED = -11;

constexpr u64 RECV_BLOCK = 1;
constexpr u64 WAIT_FOREVER = ~0ull;

constexpr u32 WAIT_READABLE = static_cast<u32>(lens::Ready::Readable);
constexpr u32 WAIT_WRITABLE = static_cast<u32>(lens::Ready::Writable);
constexpr u32 WAIT_HANGUP = static_cast<u32>(lens::Ready::Hangup);
constexpr u32 WAIT_MASK = WAIT_READABLE | WAIT_WRITABLE | WAIT_HANGUP;

struct WaitEntry {
    u64 handle;
    u32 want;
    u32 got;
};

constexpr u32 MAX_WAIT_ENTRIES = 32;

constexpr i64 error_of(lens::Status s) {
    using lens::Status;
    switch (s) {
        case Status::Ok: return 0;
        case Status::NoMemory:
        case Status::TableFull:
        case Status::TooManyEpochs:
        case Status::NoSpace: return userabi::ERR_LIMIT;
        case Status::NotReady: return ERR_AGAIN;
        case Status::Closed: return ERR_CLOSED;
        case Status::Denied:
        case Status::ReadOnly: return ERR_DENIED;
        case Status::BadLens:
        case Status::StaleLens: return ERR_BADHANDLE;
        case Status::TooLarge: return ERR_TOOBIG;
        case Status::TimedOut: return ERR_TIMEDOUT;
        case Status::Cancelled: return ERR_CANCELLED;
        case Status::NotFound:
        case Status::Exists:
        case Status::NotADirectory:
        case Status::IsADirectory:
        case Status::NotEmpty:
        case Status::Io:
        case Status::InvalidPath:
        case Status::InvalidName:
        case Status::Busy:
        case Status::Unavailable:
        case Status::Unsupported:
        case Status::Corrupt:
        case Status::CrossRealm:
        case Status::Escape:
        case Status::NotAFile:
        case Status::NoSuchEpoch:
        case Status::BatchFailed: return userabi::ERR_INVAL;
    }
    return userabi::ERR_INVAL;
}

constexpr u64 info_pack(u16 rights_bits, u8 kind) { return (static_cast<u64>(kind) << 16) | rights_bits; }
constexpr u16 info_rights(u64 info) { return static_cast<u16>(info & 0xFFFF); }
constexpr u8 info_kind(u64 info) { return static_cast<u8>((info >> 16) & 0xFF); }

constexpr i64 recv_pack(u32 bytes, u32 handles) { return static_cast<i64>((static_cast<u64>(handles) << 32) | bytes); }
constexpr u32 recv_bytes(i64 packed) { return static_cast<u32>(static_cast<u64>(packed) & 0xFFFFFFFFull); }
constexpr u32 recv_handles(i64 packed) { return static_cast<u32>(static_cast<u64>(packed) >> 32); }

constexpr bool wait_want_ok(u32 want) { return want != 0 && (want & ~WAIT_MASK) == 0; }

}
