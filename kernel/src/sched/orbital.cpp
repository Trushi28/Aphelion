#include <cosmos/orbital.hpp>
#include <cosmos/apic.hpp>
#include <cosmos/pit.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/clock.hpp>
#include <cosmos/vmm.hpp>
#include <cosmos/gdt.hpp>
#include <cosmos/fpu.hpp>

extern "C" void switch_context(u64* old_rsp_out, u64 new_rsp);

namespace orbital {

constexpr int MAX_SATELLITES = 64;
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
static cpu::Spinlock* g_release_after[MAX_CORES] = {};
static volatile u32 g_cores_on_kstack = 0;

constexpr u32 BOOST_TICKS = 50;
static u32 g_boost_clock[MAX_CORES] = {};
static volatile u64 g_boosts = 0;

static Satellite* g_sleepers = nullptr;
static cpu::Spinlock g_sleep_lock;
static volatile u64 g_next_wake = ~0ull;

enum class Leave { Requeue, Die, Park };

static void retire_dying() {
    u32 me = apic::id();
    Satellite* d = g_dying[me];
    if (d) {
        g_dying[me] = nullptr;
        g_zombie_lock.lock();
        d->next = g_zombies;
        g_zombies = d;
        g_zombie_lock.unlock();
    }
    cpu::Spinlock* hold = g_release_after[me];
    if (hold) {
        g_release_after[me] = nullptr;
        hold->unlock();
    }
}

static void finish_switch() {
    g_lock[apic::id()].unlock();
    retire_dying();
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
static void boost_locked(u32 core) {
    for (int r = 1; r < NUM_RINGS; ++r) {
        Satellite* head = g_ring_head[core][r];
        if (!head) continue;
        for (Satellite* it = head; it; it = it->next) it->ring = 0;
        if (!g_ring_head[core][0]) g_ring_head[core][0] = head;
        else g_ring_tail[core][0]->next = head;
        g_ring_tail[core][0] = g_ring_tail[core][r];
        g_ring_head[core][r] = g_ring_tail[core][r] = nullptr;
    }
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
    s->kstack_top = reinterpret_cast<u64>(stack_virt) + s->stack_size;
    s->space = nullptr;
    s->state = State::Ready;
    s->wake_at = 0;
    s->switches = 0;
    s->wait_next = nullptr;
    fpu::init_state(s->fpu);
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
    finish_switch();
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

static void arrive(Satellite* old, Satellite* next) {
    if (fpu::usable()) {
        fpu::save(old->fpu);
        fpu::restore(next->fpu);
    }
    gdt::set_kernel_stack(next->kstack_top);
    if (next->space != old->space) constellation::activate(next->space);
    ++old->switches;
    old->state = old->state == State::Running ? State::Ready : old->state;
    next->state = State::Running;
}

static void reschedule_locked(u32 me, Leave how) {
    Satellite* old = g_current[me];
    Satellite* requeue = (how == Leave::Requeue && old != &g_idle[me]) ? old : nullptr;

    Satellite* next = dequeue_highest_locked(me);
    if (!next) {
        g_lock[me].unlock();
        next = try_steal(me);
        g_lock[me].lock();
    }
    if (!next) next = requeue ? requeue : &g_idle[me];

    if (requeue && next != requeue) enqueue_locked(me, requeue);

    next->ticks_left = ticks_for_ring(next->ring);

    if (next == old) { g_lock[me].unlock(); return; }
    if (how == Leave::Die && old != &g_idle[me]) g_dying[me] = old;
    g_current[me] = next;
    arrive(old, next);
    switch_context(&old->rsp, next->rsp);
    finish_switch();
}

static void make_ready(Satellite* s) {
    u64 f = cpu::irq_save();
    u32 me = apic::id();
    s->state = State::Ready;
    s->ring = 0;
    s->ticks_left = ticks_for_ring(0);
    g_lock[me].lock();
    enqueue_locked(me, s);
    g_lock[me].unlock();
    cpu::irq_restore(f);
}

static void park_current(cpu::Spinlock* hold) {
    u32 me = apic::id();
    g_lock[me].lock();
    g_release_after[me] = hold;
    reschedule_locked(me, Leave::Park);
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
    reschedule_locked(me, Leave::Die);
    cpu::hang();
}

static void wake_due_sleepers() {
    if (__atomic_load_n(&g_next_wake, __ATOMIC_ACQUIRE) > clock::uptime_ms()) return;
    Satellite* due = nullptr;
    g_sleep_lock.lock();
    u64 now = clock::uptime_ms();
    while (g_sleepers && g_sleepers->wake_at <= now) {
        Satellite* s = g_sleepers;
        g_sleepers = s->wait_next;
        s->wait_next = due;
        due = s;
    }
    __atomic_store_n(&g_next_wake, g_sleepers ? g_sleepers->wake_at : ~0ull, __ATOMIC_RELEASE);
    g_sleep_lock.unlock();
    while (due) {
        Satellite* next_due = due->wait_next;
        due->wait_next = nullptr;
        make_ready(due);
        due = next_due;
    }
}

static void timer_tick_handler(idt::Frame*) {
    apic::eoi();
    u32 me = apic::id();
    clock::tick(me);
    Satellite* cur = g_current[me];
    if (!cur) return;

    wake_due_sleepers();

    if (++g_boost_clock[me] >= BOOST_TICKS) {
        g_boost_clock[me] = 0;
        g_lock[me].lock();
        boost_locked(me);
        g_lock[me].unlock();
        if (cur != &g_idle[me]) cur->ring = 0;
        __atomic_add_fetch(&g_boosts, 1, __ATOMIC_RELAXED);
    }

    if (cur == &g_idle[me]) {
        if (any_ready(me) || any_stealable()) { g_lock[me].lock(); reschedule_locked(me, Leave::Requeue); }
    } else if (cur->ticks_left > 0 && --cur->ticks_left == 0) {
        if (cur->ring < NUM_RINGS - 1) ++cur->ring;
        g_lock[me].lock();
        reschedule_locked(me, Leave::Requeue);
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
    if (me >= MAX_CORES) {
        serial::printf("[orbital] FATAL: local APIC id %u is beyond the %u cores the scheduler tracks\n", me, MAX_CORES);
        cpu::hang();
    }
    build_satellite(&g_idle[me], "idle", &idle_entry, nullptr, NUM_RINGS - 1, &satellite_trampoline);
    apic::start_timer_periodic(idt::VEC_APIC_TIMER, g_ticks_per_period, 4);
}

Satellite* spawn(const char* name, EntryFn entry, void* arg) {
    return spawn_in(name, entry, arg, nullptr);
}

Satellite* spawn_in(const char* name, EntryFn entry, void* arg, constellation::Space* space) {
    g_pool_lock.lock();
    Satellite* s = nullptr;
    for (auto& cand : g_pool) if (!cand.in_use) { s = &cand; break; }
    if (!s) { g_pool_lock.unlock(); return nullptr; }
    s->in_use = true;
    g_pool_lock.unlock();

    if (!build_satellite(s, name, entry, arg, 0, &satellite_trampoline)) {
        g_pool_lock.lock();
        s->in_use = false;
        g_pool_lock.unlock();
        return nullptr;
    }
    s->space = space;
    u64 f = cpu::irq_save();
    u32 me = apic::id();
    g_lock[me].lock(); enqueue_locked(me, s); g_lock[me].unlock();
    cpu::irq_restore(f);
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
    reschedule_locked(me, Leave::Requeue);
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
    reschedule_locked(me, Leave::Requeue);
    cpu::sti();
}

u64 self_token() {
    u64 flags = cpu::irq_save();
    Satellite* cur = g_current[apic::id()];
    cpu::irq_restore(flags);
    return cur ? reinterpret_cast<u64>(cur) : 1;
}

u32 cores_on_kernel_stacks() { return g_cores_on_kstack; }

Satellite* current() {
    u64 f = cpu::irq_save();
    Satellite* cur = g_current[apic::id()];
    cpu::irq_restore(f);
    return cur;
}

int current_ring() {
    Satellite* cur = current();
    return cur ? cur->ring : -1;
}

u64 boosts() { return __atomic_load_n(&g_boosts, __ATOMIC_RELAXED); }

void sleep_ms(u32 ms) {
    u64 f = cpu::irq_save();
    Satellite* cur = g_current[apic::id()];
    if (!cur || cur == &g_idle[apic::id()] || ms == 0) {
        cpu::irq_restore(f);
        if (ms == 0) yield();
        return;
    }
    g_sleep_lock.lock();
    cur->wake_at = clock::uptime_ms() + ms + clock::TICK_MS;
    cur->state = State::Sleeping;
    Satellite** link = &g_sleepers;
    while (*link && (*link)->wake_at <= cur->wake_at) link = &(*link)->wait_next;
    cur->wait_next = *link;
    *link = cur;
    __atomic_store_n(&g_next_wake, g_sleepers->wake_at, __ATOMIC_RELEASE);
    park_current(&g_sleep_lock);
    cpu::irq_restore(f);
}

void WaitQueue::wait_until(bool (*ready)(void*), void* arg) {
    for (;;) {
        u64 f = cpu::irq_save();
        lock_.lock();
        if (ready(arg)) {
            lock_.unlock();
            cpu::irq_restore(f);
            return;
        }
        Satellite* cur = g_current[apic::id()];
        if (!cur || cur == &g_idle[apic::id()]) {
            lock_.unlock();
            cpu::irq_restore(f);
            asm volatile("pause" ::: "memory");
            continue;
        }
        cur->state = State::Blocked;
        cur->wait_next = nullptr;
        if (tail_) tail_->wait_next = cur; else head_ = cur;
        tail_ = cur;
        park_current(&lock_);
        cpu::irq_restore(f);
    }
}

u32 WaitQueue::wake_one() {
    u64 f = cpu::irq_save();
    lock_.lock();
    Satellite* s = head_;
    if (s) {
        head_ = s->wait_next;
        if (!head_) tail_ = nullptr;
        s->wait_next = nullptr;
    }
    lock_.unlock();
    cpu::irq_restore(f);
    if (s) make_ready(s);
    return s ? 1 : 0;
}

u32 WaitQueue::wake_all() {
    u64 f = cpu::irq_save();
    lock_.lock();
    Satellite* list = head_;
    head_ = tail_ = nullptr;
    lock_.unlock();
    cpu::irq_restore(f);
    u32 n = 0;
    while (list) {
        Satellite* next = list->wait_next;
        list->wait_next = nullptr;
        make_ready(list);
        list = next;
        ++n;
    }
    return n;
}

u32 WaitQueue::waiters() {
    u64 f = cpu::irq_save();
    lock_.lock();
    u32 n = 0;
    for (Satellite* it = head_; it; it = it->wait_next) ++n;
    lock_.unlock();
    cpu::irq_restore(f);
    return n;
}

NORETURN void start_core() {
    u32 me = apic::id();
    g_current[me] = &g_idle[me];
    g_idle[me].state = State::Running;
    if (fpu::usable()) fpu::restore(g_idle[me].fpu);
    gdt::set_kernel_stack(g_idle[me].kstack_top);
    u64 scratch_rsp;
    switch_context(&scratch_rsp, g_idle[me].rsp);
    cpu::hang();
}

}
