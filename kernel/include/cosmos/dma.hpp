#pragma once
#include <cosmos/types.hpp>

namespace dma {

struct Buffer {
    void* virt = nullptr;
    u64 phys = 0;
    u64 bytes = 0;
    int order = -1;
    bool ok() const { return virt != nullptr; }
};

Buffer alloc(u64 bytes);
void release(Buffer& b);
u64 phys(const void* p, u64 len = 1);

}