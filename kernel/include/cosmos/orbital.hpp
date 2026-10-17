#pragma once
#include <cosmos/types.hpp>
#include <cosmos/idt.hpp>
#include <cosmos/fpu.hpp>
#include <cosmos/cpu.hpp>

namespace constellation { struct Space; }

namespace orbital {

constexpr int NUM_RINGS = 4;

using EntryFn = void (*)(void* arg);

enum class State : u32 { Ready, Running, Sleeping, Blocked };

struct Satellite {
    alignas(64) u8 fpu[fpu::STATE_MAX_BYTES];
    u64 rsp = 0;
    u64 kstack_top = 0;
    constellation::Space* space = nullptr;
    State state = State::Ready;
    u64 wake_at = 0;
    u64 switches = 0;
    Satellite* wait_next = nullptr;
    u8* stack_base = nullptr;
    u64 stack_size = 0;
    int ring = 0;
    u32 ticks_left = 0;
    bool in_use = false;
    bool guarded = false;
    EntryFn entry = nullptr;
    void* arg = nullptr;
    const char* name = "?";
    Satellite* next = nullptr;
};

void init(u64 hhdm_offset, const u32* lapic_ids, u32 core_count);

bool selftest();

void init_core();

Satellite* spawn(const char* name, EntryFn entry, void* arg);
Satellite* spawn_in(const char* name, EntryFn entry, void* arg, constellation::Space* space);

Satellite* current();
int current_ring();
u64 boosts();
void sleep_ms(u32 ms);

class WaitQueue {
public:
    void wait_until(bool (*ready)(void*), void* arg);
    u32 wake_one();
    u32 wake_all();
    u32 waiters();

private:
    cpu::Spinlock lock_;
    Satellite* head_ = nullptr;
    Satellite* tail_ = nullptr;
};

void spawn_selftest();

void yield();
void yield_contended();
NORETURN void exit_current();

NORETURN void start_core();

u64 self_token();
u32 cores_on_kernel_stacks();
class Mutex {
public:
    void lock() {
        u64 me = self_token();
        if (__atomic_load_n(&owner_, __ATOMIC_RELAXED) == me) { ++depth_; return; }
        for (;;) {
            u64 expected = 0;
            if (__atomic_compare_exchange_n(&owner_, &expected, me, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
            if (me == NO_SCHEDULER) asm volatile("pause" ::: "memory");
            else yield_contended();
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
