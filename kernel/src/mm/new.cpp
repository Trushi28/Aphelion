#include <cosmos/kheap.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/cpu.hpp>

namespace {

NORETURN void out_of_memory(usize bytes) {
    serial::printf("[kheap] FATAL: operator new could not allocate %lu bytes\n", static_cast<u64>(bytes));
    cpu::hang();
}

void* checked_alloc(usize bytes) {
    void* p = kheap::alloc(bytes);
    if (!p) out_of_memory(bytes);
    return p;
}

}

void* operator new(__SIZE_TYPE__ bytes) { return checked_alloc(bytes); }
void* operator new[](__SIZE_TYPE__ bytes) { return checked_alloc(bytes); }
void operator delete(void* p) noexcept { kheap::free(p); }
void operator delete[](void* p) noexcept { kheap::free(p); }
void operator delete(void* p, __SIZE_TYPE__) noexcept { kheap::free(p); }
void operator delete[](void* p, __SIZE_TYPE__) noexcept { kheap::free(p); }
