#pragma once
#include <cosmos/types.hpp>

namespace procgroup {

constexpr u32 LIVE_SHIFT = 32;
constexpr u64 LIVE_FIELD = 0xFFFFull << LIVE_SHIFT;
constexpr u64 DYING_BIT = 1ull << 48;
constexpr u64 STATUS_FIELD = 0xFFFFFFFFull;
constexpr u32 LIVE_MAX = 0xFFFF;

constexpr u64 pack(u32 live, bool dying, i32 status) {
    return (static_cast<u64>(live & LIVE_MAX) << LIVE_SHIFT) | (dying ? DYING_BIT : 0) | static_cast<u32>(status);
}

constexpr u32 live(u64 word) { return static_cast<u32>((word & LIVE_FIELD) >> LIVE_SHIFT); }
constexpr bool dying(u64 word) { return (word & DYING_BIT) != 0; }
constexpr i32 status(u64 word) { return static_cast<i32>(static_cast<u32>(word & STATUS_FIELD)); }
constexpr bool finished(u64 word) { return live(word) == 0; }

struct Leave {
    bool last;
    i32 status;
};

inline u64 load(const u64* word) { return __atomic_load_n(word, __ATOMIC_ACQUIRE); }

inline void start(u64* word) { __atomic_store_n(word, pack(1, false, 0), __ATOMIC_RELEASE); }

inline bool try_enter(u64* word, u32 limit) {
    u64 cur = load(word);
    for (;;) {
        if (live(cur) == 0 || dying(cur) || live(cur) >= limit || live(cur) >= LIVE_MAX) return false;
        u64 next = (cur & ~LIVE_FIELD) | (static_cast<u64>(live(cur) + 1) << LIVE_SHIFT);
        if (__atomic_compare_exchange_n(word, &cur, next, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return true;
    }
}

inline bool request_exit(u64* word, i32 value) {
    u64 cur = load(word);
    for (;;) {
        if (live(cur) == 0 || dying(cur)) return false;
        u64 next = pack(live(cur), true, value);
        if (__atomic_compare_exchange_n(word, &cur, next, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return true;
    }
}

inline Leave leave(u64* word, i32 own) {
    u64 cur = load(word);
    for (;;) {
        if (live(cur) == 0) return { false, status(cur) };
        u32 remaining = live(cur) - 1;
        i32 final_status = dying(cur) ? status(cur) : own;
        u64 next = remaining == 0 ? pack(0, dying(cur), final_status) : ((cur & ~LIVE_FIELD) | (static_cast<u64>(remaining) << LIVE_SHIFT));
        if (__atomic_compare_exchange_n(word, &cur, next, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return { remaining == 0, final_status };
    }
}

constexpr bool entry_ok(u64 rip, u64 rsp, u64 limit) { return rip != 0 && rip < limit && rsp != 0 && rsp < limit; }

}
