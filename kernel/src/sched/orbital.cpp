#include <cosmos/orbital.hpp>
#include <cosmos/apic.hpp>
#include <cosmos/pit.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/serial.hpp>

extern "C" void switch_context(u64* old_rsp_out, u64 new_rsp);

namespace orbital {

constexpr int MAX_SATELLITES = 32;
constexpr int STACK_ORDER = 3;
constexpr u32 MAX_CORES = 256;

static u64 g_hhdm = 0;
static u32 g_ticks_per_period = 0;

static Satellite g_pool[MAX_SATELLITES];
static Satellite g_idle[MAX_CORES];
static Satellite* g_current[MAX_CORES] = {};

static Satellite* g_ring_head[MAX_CORES][NUM_RINGS] = {};
static Satellite* g_ring_tail[MAX_CORES][NUM_RINGS] = {};
static cpu::Spinlock g_lock[MAX_CORES];

static Satellite* g_zombies = nullptr;
static cpu::Spinlock g_zombie_lock;
static cpu::Spinlock g_pool_lock;

static u32 ticks_for_ring(int ring) {

    static const u32 slice[NUM_RINGS] = {2, 4, 8, 16};
    return slice[ring < NUM_RINGS ? ring : NUM_RINGS - 1];
}

static void enqueue_locked(u32 core, Satellite* s) {
    s->next = nullptr;
    int r = s->ring;
    if (!g_ring_head[core][r]) g_ring_head[core][r] = g_ring_tail[core][r] = s;
    else { g_ring_tail[core][r]->next = s; g_ring_tail[core][r] = s; }
}
static Satellite* dequeue_highest_locked(u32 core) {
    for (int r = 0; r < NUM_RINGS; ++r) {
        if (g_ring_head[core][r]) {
            Satellite* s = g_ring_head[core][r];
            g_ring_head[core][r] = s->next;
            if (!g_ring_head[core][r]) g_ring_tail[core][r] = nullptr;
            return s;
        }
    }
    return nullptr;
}
static bool any_ready(u32 core) {
    for (int r = 0; r < NUM_RINGS; ++r) if (g_ring_head[core][r]) return true;
    return false;
}
static bool any_stealable() {
    for (u32 c = 0; c < MAX_CORES; ++c) if (any_ready(c)) return true;
    return false;
}

static Satellite* try_steal(u32 me) {
    for (u32 victim = 0; victim < MAX_CORES; ++victim) {
        if (victim == me) continue;
        if (!any_ready(victim)) continue;
        g_lock[victim].lock();
        Satellite* s = dequeue_highest_locked(victim);
        g_lock[victim].unlock();
        if (s) return s;
    }
    return nullptr;
}

static void build_satellite(Satellite* s, const char* name, EntryFn entry, void* arg,
                             int ring, void (*trampoline)()) {
    u64 phys = universe::alloc(STACK_ORDER);
    u8* stack_virt = reinterpret_cast<u8*>(g_hhdm + phys);
    s->stack_base = stack_virt;
    s->stack_size = universe::PAGE_SIZE << STACK_ORDER;
    s->name = name;
    s->entry = entry;
    s->arg = arg;
    s->ring = ring;
    s->ticks_left = ticks_for_ring(ring);
    s->in_use = true;

    u64* sp = reinterpret_cast<u64*>(stack_virt + s->stack_size);
    *(--sp) = 0;
    *(--sp) = reinterpret_cast<u64>(trampoline);
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    s->rsp = reinterpret_cast<u64>(sp);
}

extern "C" void satellite_trampoline() {

    g_lock[apic::id()].unlock();
    Satellite* self = g_current[apic::id()];
    cpu::sti();
    self->entry(self->arg);

    exit_current();
}

static void reap_zombies() {
    g_zombie_lock.lock();
    Satellite* z = g_zombies;
    g_zombies = nullptr;
    g_zombie_lock.unlock();

    while (z) {
        Satellite* next_z = z->next;
        serial::printf("[orbital] reaping satellite '%s' (freeing its stack)\n", z->name);
        universe::free(reinterpret_cast<u64>(z->stack_base) - g_hhdm, STACK_ORDER);
        g_pool_lock.lock();
        z->in_use = false;
        g_pool_lock.unlock();
        z = next_z;
    }
}

static void idle_entry(void*) {
    cpu::sti();
    for (;;) {
        reap_zombies();
        cpu::halt();
    }
}

static void reschedule_locked(u32 me, Satellite* old_to_enqueue) {
    Satellite* next = dequeue_highest_locked(me);
    if (!next) {
        g_lock[me].unlock();
        next = try_steal(me);
        g_lock[me].lock();
    }
    if (!next) next = old_to_enqueue ? old_to_enqueue : &g_idle[me];

    if (old_to_enqueue && next != old_to_enqueue) enqueue_locked(me, old_to_enqueue);

    next->ticks_left = ticks_for_ring(next->ring);

    Satellite* old = g_current[me];
    if (next == old) { g_lock[me].unlock(); return; }

    g_current[me] = next;
    switch_context(&old->rsp, next->rsp);
    g_lock[apic::id()].unlock();
}

NORETURN void exit_current() {
    cpu::cli();
    u32 me = apic::id();
    Satellite* cur = g_current[me];
    if (cur == &g_idle[me]) {
        cpu::sti();
        for (;;) cpu::halt();
    }

    g_zombie_lock.lock();
    cur->next = g_zombies;
    g_zombies = cur;
    g_zombie_lock.unlock();

    g_lock[me].lock();
    reschedule_locked(me, nullptr);
    cpu::hang();
}

static void timer_tick_handler(idt::Frame*) {
    apic::eoi();
    u32 me = apic::id();
    Satellite* cur = g_current[me];
    if (!cur) return;

    if (cur == &g_idle[me]) {
        if (any_ready(me) || any_stealable()) { g_lock[me].lock(); reschedule_locked(me, nullptr); }
    } else if (cur->ticks_left > 0 && --cur->ticks_left == 0) {
        if (cur->ring < NUM_RINGS - 1) ++cur->ring;
        g_lock[me].lock();
        reschedule_locked(me, cur);
    }
}

void init(u64 hhdm_offset) {
    g_hhdm = hhdm_offset;

    g_ticks_per_period = pit::calibrate_apic_ticks(10, 4);
    serial::printf("[orbital] APIC timer calibrated: %u ticks per 10ms (divide/16)\n",
                    g_ticks_per_period);
}

void init_core() {
    idt::set_handler(idt::VEC_APIC_TIMER, &timer_tick_handler);
    u32 me = apic::id();
    build_satellite(&g_idle[me], "idle", &idle_entry, nullptr, NUM_RINGS - 1, &satellite_trampoline);
    apic::start_timer_periodic(idt::VEC_APIC_TIMER, g_ticks_per_period, 4);
}

Satellite* spawn(const char* name, EntryFn entry, void* arg) {
    g_pool_lock.lock();
    Satellite* s = nullptr;
    for (auto& cand : g_pool) if (!cand.in_use) { s = &cand; break; }
    if (!s) { g_pool_lock.unlock(); return nullptr; }
    s->in_use = true;
    g_pool_lock.unlock();

    build_satellite(s, name, entry, arg, 0, &satellite_trampoline);
    u32 me = apic::id();
    g_lock[me].lock(); enqueue_locked(me, s); g_lock[me].unlock();
    return s;
}

void yield() {
    cpu::cli();
    u32 me = apic::id();
    Satellite* cur = g_current[me];
    if (cur == &g_idle[me]) {

        cpu::sti();
        return;
    }
    if (cur->ring > 0) --cur->ring;
    g_lock[me].lock();
    reschedule_locked(me, cur);
    cpu::sti();
}

void yield_contended() {
    cpu::cli();
    u32 me = apic::id();
    Satellite* cur = g_current[me];
    if (cur == &g_idle[me]) {
        cpu::sti();
        return;
    }
    cur->ring = NUM_RINGS - 1;
    g_lock[me].lock();
    reschedule_locked(me, cur);
    cpu::sti();
}

u64 self_token() {
    u64 flags = cpu::irq_save();
    Satellite* cur = g_current[apic::id()];
    cpu::irq_restore(flags);
    return cur ? reinterpret_cast<u64>(cur) : 1;
}

NORETURN void start_core() {
    u32 me = apic::id();
    g_current[me] = &g_idle[me];
    u64 scratch_rsp;
    switch_context(&scratch_rsp, g_idle[me].rsp);
    cpu::hang();
}

}
