#pragma once
#include <cosmos/types.hpp>

namespace lens {

enum class Right : u16 {
    Read      = 1u << 0,
    Write     = 1u << 1,
    Append    = 1u << 2,
    Enumerate = 1u << 3,
    Create    = 1u << 4,
    Unlink    = 1u << 5,
    Rename    = 1u << 6,
    Freeze    = 1u << 7,
    Delegate  = 1u << 8,
};

constexpr u32 RIGHT_COUNT = 9;
constexpr u16 RIGHTS_ALL_BITS = (1u << RIGHT_COUNT) - 1;

struct Rights {
    u16 bits;

    constexpr Rights() : bits(0) {}
    constexpr Rights(Right r) : bits(static_cast<u16>(r)) {}
    static constexpr Rights none() { return Rights(); }
    static constexpr Rights all() { Rights r; r.bits = RIGHTS_ALL_BITS; return r; }

    static constexpr Rights from_bits(u32 raw) { Rights r; r.bits = static_cast<u16>(raw & RIGHTS_ALL_BITS); return r; }

    constexpr bool has(Right r) const { return (bits & static_cast<u16>(r)) == static_cast<u16>(r); }
    constexpr bool has_all(Rights need) const { return (bits & need.bits) == need.bits; }
    constexpr bool has_any(Rights any) const { return (bits & any.bits) != 0; }
    constexpr bool empty() const { return bits == 0; }

    constexpr bool subset_of(Rights parent) const { return (bits & ~parent.bits) == 0; }

    constexpr Rights operator|(Rights o) const { return from_bits(static_cast<u32>(bits) | o.bits); }
    constexpr Rights operator&(Rights o) const { return from_bits(static_cast<u32>(bits) & o.bits); }

    constexpr Rights operator~() const { return from_bits(~static_cast<u32>(bits)); }
    constexpr Rights& operator|=(Rights o) { *this = *this | o; return *this; }
    constexpr Rights& operator&=(Rights o) { *this = *this & o; return *this; }
    constexpr bool operator==(const Rights&) const = default;
};

constexpr Rights operator|(Right a, Right b) { return Rights(a) | Rights(b); }
constexpr Rights operator|(Right a, Rights b) { return Rights(a) | b; }

constexpr Rights RIGHTS_READ_ONLY = Right::Read | Right::Enumerate;
constexpr Rights RIGHTS_READ_WRITE = Right::Read | Right::Write | Right::Append | Right::Enumerate | Right::Create
                                     | Right::Unlink | Right::Rename;

inline u32 rights_text(Rights r, char* out, u32 cap) {
    static constexpr char LETTERS[RIGHT_COUNT] = {'r', 'w', 'a', 'e', 'c', 'u', 'n', 'f', 'd'};
    u32 n = 0;
    for (u32 i = 0; i < RIGHT_COUNT; ++i) {
        if (n + 1 < cap) out[n] = (r.bits & (1u << i)) ? LETTERS[i] : '-';
        ++n;
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
    return n;
}

enum class Status : i32 {
    Ok = 0,
    NotFound,
    Exists,
    NotADirectory,
    IsADirectory,
    NotEmpty,
    NoSpace,
    NoMemory,
    Io,
    InvalidPath,
    InvalidName,
    ReadOnly,
    Busy,
    TooLarge,
    Unavailable,
    Unsupported,
    Corrupt,
    CrossRealm,
    Denied,
    Escape,
    StaleLens,
    BadLens,
    TableFull,
    NotAFile,
    NoSuchEpoch,
    TooManyEpochs,
    BatchFailed,
    TimedOut,
    NotReady,
    Closed,
    Cancelled,
};

constexpr const char* status_name(Status s) {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::NotFound: return "not found";
        case Status::Exists: return "already exists";
        case Status::NotADirectory: return "not a directory";
        case Status::IsADirectory: return "is a directory";
        case Status::NotEmpty: return "directory is not empty";
        case Status::NoSpace: return "no space left";
        case Status::NoMemory: return "out of memory";
        case Status::Io: return "input/output error";
        case Status::InvalidPath: return "invalid path";
        case Status::InvalidName: return "invalid name";
        case Status::ReadOnly: return "read-only";
        case Status::Busy: return "busy";
        case Status::TooLarge: return "too large";
        case Status::Unavailable: return "not available";
        case Status::Unsupported: return "not supported";
        case Status::Corrupt: return "data failed its checksum";
        case Status::CrossRealm: return "cannot cross realms";
        case Status::Denied: return "lens lacks the right";
        case Status::Escape: return "path leaves the lens";
        case Status::StaleLens: return "stale lens";
        case Status::BadLens: return "bad lens";
        case Status::TableFull: return "lens table full";
        case Status::NotAFile: return "not a file";
        case Status::NoSuchEpoch: return "no such epoch";
        case Status::TooManyEpochs: return "too many epochs";
        case Status::BatchFailed: return "batch rolled back";
        case Status::TimedOut: return "timed out";
        case Status::NotReady: return "not ready";
        case Status::Closed: return "stream closed";
        case Status::Cancelled: return "cancelled";
    }
    return "unknown";
}

enum class Ready : u8 {
    Readable = 1u << 0,
    Writable = 1u << 1,
    Hangup   = 1u << 2,
};

struct Readiness {
    u8 bits;

    constexpr Readiness() : bits(0) {}
    constexpr Readiness(Ready r) : bits(static_cast<u8>(r)) {}
    constexpr bool has(Ready r) const { return (bits & static_cast<u8>(r)) != 0; }
    constexpr bool any() const { return bits != 0; }
    constexpr bool intersects(Readiness o) const { return (bits & o.bits) != 0; }
    constexpr Readiness operator|(Readiness o) const { Readiness r; r.bits = static_cast<u8>(bits | o.bits); return r; }
    constexpr Readiness operator&(Readiness o) const { Readiness r; r.bits = static_cast<u8>(bits & o.bits); return r; }
    constexpr bool operator==(const Readiness&) const = default;
};
constexpr Readiness operator|(Ready a, Ready b) { return Readiness(a) | Readiness(b); }

template <typename T>
class [[nodiscard]] Result {
public:
    constexpr Result() : st_(Status::Ok), value_() {}
    static constexpr Result success(T v) { Result r; r.value_ = v; return r; }
    static constexpr Result fail(Status s) { Result r; r.st_ = s; return r; }

    constexpr bool ok() const { return st_ == Status::Ok; }
    constexpr explicit operator bool() const { return ok(); }
    constexpr Status status() const { return st_; }

    constexpr const T& value() const { return value_; }
    constexpr T& value() { return value_; }
    constexpr T value_or(T fallback) const { return ok() ? value_ : fallback; }

private:
    Status st_;
    T value_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    constexpr Result() : st_(Status::Ok) {}
    static constexpr Result success() { return Result(); }
    static constexpr Result fail(Status s) { Result r; r.st_ = s; return r; }

    constexpr bool ok() const { return st_ == Status::Ok; }
    constexpr explicit operator bool() const { return ok(); }
    constexpr Status status() const { return st_; }

private:
    Status st_;
};

using Done = Result<void>;

template <typename T>
struct Span {
    T* ptr;
    usize len;

    constexpr Span() : ptr(nullptr), len(0) {}
    constexpr Span(T* p, usize n) : ptr(p), len(n) {}

    template <typename U>
        requires(__is_same(const U, T) && !__is_same(U, T))
    constexpr Span(Span<U> o) : ptr(o.ptr), len(o.len) {}

    constexpr T* data() const { return ptr; }
    constexpr usize size() const { return len; }
    constexpr bool empty() const { return len == 0; }
    constexpr T* begin() const { return ptr; }
    constexpr T* end() const { return ptr + len; }
    constexpr T& operator[](usize i) const { return ptr[i]; }

    constexpr Span subspan(usize off, usize n = ~static_cast<usize>(0)) const {
        if (off >= len) return Span(ptr + len, 0);
        usize room = len - off;
        return Span(ptr + off, n < room ? n : room);
    }
    constexpr Span first(usize n) const { return Span(ptr, n < len ? n : len); }
};

using Bytes = Span<u8>;
using ConstBytes = Span<const u8>;

template <typename T>
constexpr Span<T> span_of(T* p, usize n) { return Span<T>(p, n); }

inline ConstBytes as_bytes(const void* p, usize n) { return ConstBytes(static_cast<const u8*>(p), n); }
inline Bytes as_writable_bytes(void* p, usize n) { return Bytes(static_cast<u8*>(p), n); }

inline ConstBytes text_bytes(const char* s) {
    usize n = 0;
    while (s[n]) ++n;
    return ConstBytes(reinterpret_cast<const u8*>(s), n);
}

inline usize copy_bytes(Bytes dst, ConstBytes src) {
    usize n = dst.len < src.len ? dst.len : src.len;
    for (usize i = 0; i < n; ++i) dst.ptr[i] = src.ptr[i];
    return n;
}

struct Epoch {
    u64 id;

    constexpr Epoch() : id(0) {}
    constexpr explicit Epoch(u64 snapshot_id) : id(snapshot_id) {}
    static constexpr Epoch none() { return Epoch(); }
    constexpr bool valid() const { return id != 0; }
    constexpr u64 raw() const { return id; }
    constexpr bool operator==(const Epoch&) const = default;
    constexpr bool operator<(const Epoch& o) const { return id < o.id; }
};

}
