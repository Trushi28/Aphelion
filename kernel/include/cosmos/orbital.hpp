#pragma once
#include <cosmos/types.hpp>
#include <cosmos/idt.hpp>
#include <cosmos/fpu.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/mlfq.hpp>

namespace constellation { struct Space; }

namespace orbital {

constexpr int NUM_RINGS = mlfq::RINGS;
constexpr u32 NAME_BYTES = 24;
constexpr u64 FOREVER = ~0ull;

using EntryFn = void (*)(void* arg);

enum class State : u32 { Ready, Running, Sleeping, Blocked };

class WaitQueue;

struct Satellite : mlfq::Account {
    alignas(64) u8 fpu[fpu::STATE_MAX_BYTES];
    u64 rsp = 0;
    u64 kstack_top = 0;
    constellation::Space* space = nullptr;
    State state = State::Ready;
    u64 wake_at = 0;
    u64 switches = 0;
    Satellite* wait_next = nullptr;
    Satellite* sleep_next = nullptr;
    WaitQueue* waiting_on = nullptr;
    u8* stack_base = nullptr;
    u64 stack_size = 0;
    u32 epoch = 0;
    u32 claimed_epoch = 0;
    u32 id = 0;
    i32 exit_status = 0;
    u32 last_core = 0;
    u64 run_ticks = 0;
    u64 created_ms = 0;
    u64 blocked_ms = 0;
    bool in_use = false;
    bool guarded = false;
    bool timed = false;
    bool has_deadline = false;
    EntryFn entry = nullptr;
    void* arg = nullptr;
    const char* name = nullptr;
    Satellite* next = nullptr;
};

struct SatelliteInfo {
    u32 id;
    State state;
    int ring;
    u32 core;
    u64 switches;
    u64 run_ticks;
    u64 age_ms;
    char name[NAME_BYTES];
};

struct CoreInfo {
    u32 apic_id;
    bool idle;
    u32 queued;
    u64 busy_ticks;
    u64 idle_ticks;
    char running[NAME_BYTES];
};

struct Stats {
    u64 boosts;
    u64 preemptions;
    u64 idle_wakeups;
    u64 steals;
    u64 ipis_sent;
    u64 spawned;
    u64 exited;
};

enum class JoinResult : u8 { Exited, TimedOut, Unknown, Self };

void init(u64 hhdm_offset, const u32* lapic_ids, u32 core_count);

bool selftest();

void init_core();

Satellite* spawn(const char* name, EntryFn entry, void* arg);
Satellite* spawn_in(const char* name, EntryFn entry, void* arg, constellation::Space* space);

Satellite* current();
u32 current_id();
int current_ring();
u64 boosts();
void sleep_ms(u32 ms);

u32 snapshot(SatelliteInfo* out, u32 max);
u32 cores(CoreInfo* out, u32 max);
Stats stats();

JoinResult join(u32 id, i32* status, u64 timeout_ms);

class WaitQueue {
public:
    void wait_until(bool (*ready)(void*), void* arg);
    bool wait_until_for(bool (*ready)(void*), void* arg, u64 timeout_ms);
    u32 wake_one();
    u32 wake_all();
    u32 waiters();
    bool detach(Satellite* s);

private:
    cpu::Spinlock lock_;
    Satellite* head_ = nullptr;
    Satellite* tail_ = nullptr;
};

class Semaphore {
public:
    void acquire();
    bool try_acquire();
    bool acquire_for(u64 timeout_ms);
    void release(u32 n = 1);
    u32 value() const { return __atomic_load_n(&count_, __ATOMIC_RELAXED); }

private:
    static bool positive(void* self);

    WaitQueue queue_;
    u32 count_ = 0;
};

class SleepLock {
public:
    void lock();
    bool try_lock();
    bool lock_for(u64 timeout_ms);
    void unlock();
    bool held_by_me() const;

private:
    static bool is_free(void* self);
    bool take(u64 me);

    WaitQueue queue_;
    u64 owner_ = 0;
};

void spawn_selftest();

void yield();
void yield_contended();
NORETURN void exit_current();
NORETURN void exit_with(i32 status);

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
        if (!held_by_me()) cpu::hang();
        if (--depth_ == 0) __atomic_store_n(&owner_, 0, __ATOMIC_RELEASE);
    }
    bool held_by_me() const { return __atomic_load_n(&owner_, __ATOMIC_RELAXED) == self_token(); }

private:
    static constexpr u64 NO_SCHEDULER = 1;
    u64 owner_ = 0;
    u32 depth_ = 0;
};

}
