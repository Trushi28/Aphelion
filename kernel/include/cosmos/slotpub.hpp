#pragma once
#include <cosmos/types.hpp>

namespace slotpub {

template <u32 N>
struct Roster {
    volatile u32 reserved;
    volatile u32 ready[N];

    void reset() {
        __atomic_store_n(&reserved, 0u, __ATOMIC_RELAXED);
        for (u32 i = 0; i < N; ++i) __atomic_store_n(&ready[i], 0u, __ATOMIC_RELAXED);
    }

    u32 reserve() {
        u32 index = __atomic_load_n(&reserved, __ATOMIC_ACQUIRE);
        for (;;) {
            if (index >= N) return N;
            if (__atomic_compare_exchange_n(&reserved, &index, index + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return index;
        }
    }

    void publish(u32 index) { __atomic_store_n(&ready[index], 1u, __ATOMIC_RELEASE); }

    bool visible(u32 index) const { return index < N && __atomic_load_n(&ready[index], __ATOMIC_ACQUIRE) != 0; }

    u32 count() const { return __atomic_load_n(&reserved, __ATOMIC_ACQUIRE); }
};

}
