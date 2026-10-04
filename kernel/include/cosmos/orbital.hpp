#pragma once
#include <cosmos/types.hpp>
#include <cosmos/idt.hpp>

namespace orbital {

constexpr int NUM_RINGS = 4;

using EntryFn = void (*)(void* arg);

struct Satellite {
    u64 rsp = 0;
    u8* stack_base = nullptr;
    u64 stack_size = 0;
    int ring = 0;
    u32 ticks_left = 0;
    bool in_use = false;
    EntryFn entry = nullptr;
    void* arg = nullptr;
    const char* name = "?";
    Satellite* next = nullptr;
};

void init(u64 hhdm_offset);

void init_core();

Satellite* spawn(const char* name, EntryFn entry, void* arg);

void yield();
NORETURN void exit_current();

NORETURN void start_core();

u64 self_token();

class Mutex {
public:
    void lock() {
        u64 me = self_token();
        if (__atomic_load_n(&owner_, __ATOMIC_RELAXED) == me) { ++depth_; return; }
        for (;;) {
            u64 expected = 0;
            if (__atomic_compare_exchange_n(&owner_, &expected, me, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
            if (me == NO_SCHEDULER) asm volatile("pause" ::: "memory");
            else yield();
        }
        depth_ = 1;
    }
    void unlock() {
        if (--depth_ == 0) __atomic_store_n(&owner_, 0, __ATOMIC_RELEASE);
    }
    bool held_by_me() const { return __atomic_load_n(&owner_, __ATOMIC_RELAXED) == self_token(); }

private:
    static constexpr u64 NO_SCHEDULER = 1;
    u64 owner_ = 0;
    u32 depth_ = 0;
};

}
