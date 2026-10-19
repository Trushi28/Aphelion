#pragma once
#include <cosmos/types.hpp>

namespace klog {

template <u32 N>
class Ring {
public:
    static_assert((N & (N - 1)) == 0, "capacity must be a power of two");

    void put(char c) {
        buf_[head_ & (N - 1)] = c;
        ++head_;
    }

    u64 tail_seq() const { return head_; }
    u32 size() const { return head_ < N ? static_cast<u32>(head_) : N; }
    u64 oldest_seq() const { return head_ > N ? head_ - N : 0; }
    u64 dropped(u64 from) const { return from < oldest_seq() ? oldest_seq() - from : 0; }

    u32 read(u64& seq, char* out, u32 max) const {
        if (seq < oldest_seq()) seq = oldest_seq();
        u32 n = 0;
        while (n < max && seq < head_) out[n++] = buf_[(seq++) & (N - 1)];
        return n;
    }

private:
    char buf_[N];
    u64 head_ = 0;
};

constexpr u32 CAPACITY = 16384;

void put(char c);
u64 tail();
u32 read(u64& seq, char* out, u32 max);
u64 dropped(u64 from);

}
