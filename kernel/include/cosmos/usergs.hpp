#pragma once
#include <cosmos/types.hpp>

namespace usergs {

constexpr u32 MSR_KERNEL_GS_BASE = 0xC0000102;

constexpr bool base_ok(u64 base, u64 limit) { return base < limit; }

template <typename Msr>
inline void switch_to(Msr& msr, bool old_is_user, u64* old_slot, bool next_is_user, u64 next_value) {
    if (!old_is_user && !next_is_user) return;
    u64 live = 0;
    if (old_is_user) {
        live = msr.read();
        *old_slot = live;
    }
    u64 want = next_is_user ? next_value : 0;
    if (old_is_user && live == want) return;
    msr.write(want);
}

}
