#include <cosmos/orbital.hpp>
#include <cosmos/apic.hpp>
#include <cosmos/pit.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/clock.hpp>
#include <cosmos/vmm.hpp>

extern "C" void switch_context(u64* old_rsp_out, u64 new_rsp);

namespace orbital {

constexpr int MAX_SATELLITES = 32;
constexpr int STACK_ORDER = 3;
constexpr u32 MAX_CORES = 256;
constexpr u32 IDLE_SLOT_BASE = MAX_SATELLITES;

static_assert((universe::PAGE_SIZE << STACK_ORDER) == constellation::STACK_BYTES, "stack size must match the guarded slot layout");
static_assert(IDLE_SLOT_BASE + MAX_CORES <= constellation::STACK_SLOTS, "not enough guarded stack slots");

static u64 g_hhdm = 0;
static u32 g_ticks_per_period = 0;

static Satellite g_pool[MAX_SATELLITES];
static Satellite g_idle[MAX_CORES];
static bool g_slot_ready[IDLE_SLOT_BASE + MAX_CORES];
static Satellite* g_current[MAX_CORES] = {};

static Satellite* g_ring_head[MAX_CORES][NUM_RINGS] = {};
static Satellite* g_ring_tail[MAX_CORES][NUM_RINGS] = {};
static cpu::Spinlock g_lock[MAX_CORES];

static Satellite* g_zombies = nullptr;
static cpu::Spinlock g_zombie_lock;
static cpu::Spinlock g_pool_lock;

static Satellite* g_dying[MAX_CORES] = {};
static volatile u32 g_cores_on_kstack = 0;

static void retire_dying() {
    u32 me = apic::id();
    Satellite* d = g_dying[me];
    if (!d) return;
    g_dying[me] = nullptr;
    g_zombie_lock.lock();
    d->next = g_zombies;
    g_zombies = d;
    g_zombie_lock.unlock();
}

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

static u32 slot_of(const Satellite* s) {
    if (s >= g_pool && s < g_pool + MAX_SATELLITES) return static_cast<u32>(s - g_pool);
    return IDLE_SLOT_BASE + static_cast<u32>(s - g_idle);
}

static bool premap_slot(u32 slot) {
    u64 phys = universe::alloc(STACK_ORDER);
    if (!phys) return false;
    if (!constellation::map_range(constellation::stack_slot_bottom(slot), phys, constellation::STACK_BYTES,
                                  constellation::WRITABLE | constellation::NO_EXECUTE)) {
        universe::free(phys, STACK_ORDER);
        return false;
    }
    g_slot_ready[slot] = true;
    return true;
}

static bool build_satellite(Satellite* s, const char* name, EntryFn entry, void* arg,
                             int ring, void (*trampoline)()) {
    u32 slot = slot_of(s);
    u8* stack_virt;
    if (g_slot_ready[slot]) {
        stack_virt = reinterpret_cast<u8*>(constellation::stack_slot_bottom(slot));
        s->guarded = true;
    } else {
        u64 phys = universe::alloc(STACK_ORDER);
        if (!phys) return false;
        stack_virt = reinterpret_cast<u8*>(g_hhdm + phys);
        s->guarded = false;
    }
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
    return true;
}

extern "C" void satellite_trampoline() {

    g_lock[apic::id()].unlock();
    retire_dying();
    Satellite* self = g_current[apic::id()];
    cpu::sti();
    self->entry(self->arg);

    exit_current();
}

static void reap_zombies() {
    u64 f = cpu::irq_save();
    g_zombie_lock.lock();
    Satellite* z = g_zombies;
    g_zombies = nullptr;
    g_zombie_lock.unlock();
    cpu::irq_restore(f);

    while (z) {
        Satellite* next_z = z->next;
        serial::printf("[orbital] reaping satellite '%s' (%s)\n", z->name,
                        z->guarded ? "stack slot returned" : "freeing its stack");
        if (!z->guarded) universe::free(reinterpret_cast<u64>(z->stack_base) - g_hhdm, STACK_ORDER);
        g_pool_lock.lock();
        z->in_use = false;
        g_pool_lock.unlock();
        z = next_z;
    }
}

static void idle_entry(void*) {
    __atomic_add_fetch(&g_cores_on_kstack, 1u, __ATOMIC_SEQ_CST);
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
    if (!old_to_enqueue && old != &g_idle[me]) g_dying[me] = old;
    g_current[me] = next;
    switch_context(&old->rsp, next->rsp);
    g_lock[apic::id()].unlock();
    retire_dying();
}

NORETURN void exit_current() {
    cpu::cli();
    u32 me = apic::id();
    Satellite* cur = g_current[me];
    if (cur == &g_idle[me]) {
        cpu::sti();
        for (;;) cpu::halt();
    }

    g_lock[me].lock();
    reschedule_locked(me, nullptr);
    cpu::hang();
}

static void timer_tick_handler(idt::Frame*) {
    apic::eoi();
    u32 me = apic::id();
    clock::tick(me);
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

void init(u64 hhdm_offset, const u32* lapic_ids, u32 core_count) {
    g_hhdm = hhdm_offset;

    u32 mapped = 0, missing = 0;
    for (u32 i = 0; i < MAX_SATELLITES; ++i) (premap_slot(i) ? mapped : missing) += 1;
    for (u32 i = 0; i < core_count; ++i) {
        if (lapic_ids[i] >= MAX_CORES) { ++missing; continue; }
        (premap_slot(IDLE_SLOT_BASE + lapic_ids[i]) ? mapped : missing) += 1;
    }
    serial::printf("[orbital] %u guarded stack slot(s) mapped, %u without a guard\n", mapped, missing);

    g_ticks_per_period = pit::calibrate_apic_ticks(clock::TICK_MS, 4);
    serial::printf("[orbital] APIC timer calibrated: %u ticks per %ums (divide/16)\n",
                    g_ticks_per_period, clock::TICK_MS);
}

bool selftest() {
    bool ok = true;
    constellation::Mapping m{};
    for (u32 slot = 0; slot < IDLE_SLOT_BASE + MAX_CORES; ++slot) {
        if (!g_slot_ready[slot]) continue;
        u64 bottom = constellation::stack_slot_bottom(slot);
        u64 top = constellation::stack_slot_top(slot);
        ok = ok && !constellation::translate(constellation::stack_slot_base(slot), &m);
        ok = ok && !constellation::translate(top, &m);
        ok = ok && constellation::translate(bottom, &m) && (m.flags & constellation::WRITABLE);
        ok = ok && constellation::translate(top - universe::PAGE_SIZE, &m) && (m.flags & constellation::WRITABLE);
        ok = ok && constellation::in_stack_guard(constellation::stack_slot_base(slot) + 8);
        ok = ok && !constellation::in_stack_guard(bottom);
    }
    return ok;
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

u32 cores_on_kernel_stacks() { return g_cores_on_kstack; }

NORETURN void start_core() {
    u32 me = apic::id();
    g_current[me] = &g_idle[me];
    u64 scratch_rsp;
    switch_context(&scratch_rsp, g_idle[me].rsp);
    cpu::hang();
}

}
