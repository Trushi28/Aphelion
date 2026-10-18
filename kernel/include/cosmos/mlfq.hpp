#pragma once
#include <cosmos/types.hpp>

namespace mlfq {

constexpr int RINGS = 4;
constexpr u32 SLICE[RINGS] = { 2, 4, 8, 16 };
constexpr u32 ALLOTMENT[RINGS] = { 8, 16, 32, 0 };
constexpr u32 WAKE_PROMOTE_TICKS = 5;

enum class Verdict : u8 { Continue, Rotate, Demote };

struct Account {
    int ring = 0;
    u32 slice_left = 0;
    u32 allot_left = 0;
};

constexpr int clamp_ring(int r) { return r < 0 ? 0 : (r >= RINGS ? RINGS - 1 : r); }

constexpr void enter(Account& a, int ring) {
    a.ring = clamp_ring(ring);
    a.slice_left = SLICE[a.ring];
    a.allot_left = ALLOTMENT[a.ring];
}

constexpr void dispatch(Account& a) { a.slice_left = SLICE[clamp_ring(a.ring)]; }

constexpr Verdict on_tick(Account& a) {
    a.ring = clamp_ring(a.ring);
    if (ALLOTMENT[a.ring] != 0) {
        if (a.allot_left > 0) --a.allot_left;
        if (a.allot_left == 0 && a.ring < RINGS - 1) {
            enter(a, a.ring + 1);
            return Verdict::Demote;
        }
    }
    if (a.slice_left > 0) --a.slice_left;
    if (a.slice_left == 0) {
        a.slice_left = SLICE[a.ring];
        return Verdict::Rotate;
    }
    return Verdict::Continue;
}

constexpr void on_wake(Account& a, u64 slept_ticks) {
    if (slept_ticks >= WAKE_PROMOTE_TICKS && a.ring > 0) enter(a, a.ring - 1);
}

constexpr void on_boost(Account& a) { enter(a, 0); }

constexpr void pin_lowest(Account& a) { enter(a, RINGS - 1); }

constexpr bool preempts(int woken_ring, int running_ring) { return woken_ring < running_ring; }

static_assert(SLICE[0] > 0 && SLICE[RINGS - 1] > 0, "every ring needs a time slice");
static_assert(ALLOTMENT[RINGS - 1] == 0, "the last ring must not demote any further");

}
