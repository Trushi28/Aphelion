#pragma once
#include <cosmos/types.hpp>

namespace startup {

constexpr u32 MAGIC = 0x54535041u;
constexpr u16 VERSION = 1;
constexpr u32 MAX_MESSAGE = 256;
constexpr u32 MAX_HANDLES = 4;
constexpr u32 MAX_ARGS = 16;
constexpr u32 MAX_ENV = 8;

constexpr u32 PROGRAM = 0;
constexpr u32 DIRECTORY = 1;
constexpr u32 STDIN = 2;
constexpr u32 STDOUT = 3;
constexpr u32 SLOT_COUNT = 4;
constexpr u32 SLOT_MASK = (1u << SLOT_COUNT) - 1u;

constexpr i32 OK = 0;
constexpr i32 ERR_TOO_MANY = -1;
constexpr i32 ERR_TOO_LARGE = -2;
constexpr i32 ERR_MALFORMED = -3;
constexpr i32 ERR_VERSION = -4;

struct Header {
    u32 magic;
    u16 version;
    u16 argc;
    u16 envc;
    u16 present;
    u16 text;
    u16 reserved;
};

static_assert(sizeof(Header) == 16);

constexpr u32 MAX_TEXT = MAX_MESSAGE - sizeof(Header);

struct Parsed {
    u32 argc;
    u32 envc;
    u32 present;
    const char* argv[MAX_ARGS + 1];
    const char* env[MAX_ENV + 1];
};

constexpr u32 count_bits(u32 mask) {
    u32 n = 0;
    while (mask) {
        n += mask & 1u;
        mask >>= 1;
    }
    return n;
}

constexpr bool has(u32 present, u32 slot) { return slot < SLOT_COUNT && (present & (1u << slot)) != 0; }

constexpr i32 handle_index(u32 present, u32 slot) {
    if (!has(present, slot)) return -1;
    return static_cast<i32>(count_bits(present & ((1u << slot) - 1u)));
}

namespace detail {

inline u32 text_length(const char* s) {
    u32 n = 0;
    while (s[n]) ++n;
    return n;
}

inline bool put(u8* out, u32 cap, u32* used, const char* s) {
    u32 n = text_length(s) + 1;
    if (n > cap - *used) return false;
    for (u32 i = 0; i < n; ++i) out[*used + i] = static_cast<u8>(s[i]);
    *used += n;
    return true;
}

}

inline i32 encode(const char* const* argv, u32 argc, const char* const* env, u32 envc, u32 present, u8* out, u32 cap, u32* length) {
    *length = 0;
    if (argc > MAX_ARGS || envc > MAX_ENV) return ERR_TOO_MANY;
    if ((present & ~SLOT_MASK) != 0 || cap < sizeof(Header)) return ERR_MALFORMED;
    if ((argc && !argv) || (envc && !env)) return ERR_MALFORMED;
    u32 used = sizeof(Header);
    for (u32 i = 0; i < argc; ++i) {
        if (!argv[i]) return ERR_MALFORMED;
        if (!detail::put(out, cap < MAX_MESSAGE ? cap : MAX_MESSAGE, &used, argv[i])) return ERR_TOO_LARGE;
    }
    for (u32 i = 0; i < envc; ++i) {
        if (!env[i]) return ERR_MALFORMED;
        if (!detail::put(out, cap < MAX_MESSAGE ? cap : MAX_MESSAGE, &used, env[i])) return ERR_TOO_LARGE;
    }
    Header h{};
    h.magic = MAGIC;
    h.version = VERSION;
    h.argc = static_cast<u16>(argc);
    h.envc = static_cast<u16>(envc);
    h.present = static_cast<u16>(present);
    h.text = static_cast<u16>(used - sizeof(Header));
    h.reserved = 0;
    for (u32 i = 0; i < sizeof(Header); ++i) out[i] = reinterpret_cast<const u8*>(&h)[i];
    *length = used;
    return OK;
}

inline i32 decode(const u8* bytes, u32 length, u32 handles, Parsed* out) {
    out->argc = out->envc = out->present = 0;
    out->argv[0] = nullptr;
    out->env[0] = nullptr;
    if (!bytes || length < sizeof(Header) || length > MAX_MESSAGE) return ERR_MALFORMED;
    Header h;
    for (u32 i = 0; i < sizeof(Header); ++i) reinterpret_cast<u8*>(&h)[i] = bytes[i];
    if (h.magic != MAGIC) return ERR_MALFORMED;
    if (h.version != VERSION) return ERR_VERSION;
    if (h.argc > MAX_ARGS || h.envc > MAX_ENV) return ERR_TOO_MANY;
    if (h.reserved != 0 || (h.present & ~SLOT_MASK) != 0) return ERR_MALFORMED;
    if (static_cast<u32>(h.text) != length - sizeof(Header)) return ERR_MALFORMED;
    if (count_bits(h.present) != handles) return ERR_MALFORMED;
    const u32 strings = static_cast<u32>(h.argc) + h.envc;
    if (h.text != 0 && bytes[length - 1] != 0) return ERR_MALFORMED;
    u32 at = sizeof(Header);
    u32 found = 0;
    while (at < length) {
        if (found == strings) return ERR_MALFORMED;
        const char* s = reinterpret_cast<const char*>(bytes + at);
        if (found < h.argc) out->argv[found] = s;
        else out->env[found - h.argc] = s;
        ++found;
        at += detail::text_length(s) + 1;
    }
    if (found != strings) return ERR_MALFORMED;
    out->argc = h.argc;
    out->envc = h.envc;
    out->present = h.present;
    out->argv[h.argc] = nullptr;
    out->env[h.envc] = nullptr;
    return OK;
}

}
