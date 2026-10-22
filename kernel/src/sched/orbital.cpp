#include <cosmos/orbital.hpp>
#include <cosmos/apic.hpp>
#include <cosmos/timesrc.hpp>
#include <cosmos/bootcfg.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/clock.hpp>
#include <cosmos/vmm.hpp>
#include <cosmos/gdt.hpp>
#include <cosmos/fpu.hpp>
#include <cosmos/runq.hpp>

extern "C" void switch_context(u64* old_rsp_out, u64 new_rsp);

namespace orbital {

constexpr int MAX_SATELLITES = 64;
constexpr int STACK_ORDER = 3;
constexpr u32 MAX_CORES = 256;
constexpr u32 IDLE_SLOT_BASE = MAX_SATELLITES;
constexpr u32 EXIT_RING = 32;

static_assert((universe::PAGE_SIZE << STACK_ORDER) == constellation::STACK_BYTES, "stack size must match the guarded slot layout");
static_assert(IDLE_SLOT_BASE + MAX_CORES <= constellation::STACK_SLOTS, "not enough guarded stack slots");

using RunQueue = runq::Queue<Satellite, mlfq::RINGS>;

struct ExitRecord {
    u32 id;
    i32 status;
    u64 run_ticks;
    char name[NAME_BYTES];
};

static u64 g_hhdm = 0;
static u32 g_ticks_per_period = 0;

static Satellite g_pool[MAX_SATELLITES];
static Satellite g_idle[MAX_CORES];
static bool g_slot_ready[IDLE_SLOT_BASE + MAX_CORES];
static Satellite* g_current[MAX_CORES] = {};

static RunQueue g_runq[MAX_CORES];
static cpu::Spinlock g_lock[MAX_CORES];

static u32 g_core_ids[MAX_CORES];
static volatile u32 g_core_count = 0;
static volatile u8 g_core_idle[MAX_CORES] = {};
static volatile u32 g_rotor = 0;

static u64 g_busy_ticks[MAX_CORES] = {};
static u64 g_idle_ticks[MAX_CORES] = {};

static Satellite* g_zombies = nullptr;
static cpu::Spinlock g_zombie_lock;
static cpu::Spinlock g_pool_lock;

static Satellite* g_dying[MAX_CORES] = {};
static cpu::Spinlock* g_release_after[MAX_CORES] = {};
static volatile u32 g_cores_on_kstack = 0;

constexpr u32 BOOST_TICKS = 50;
static u32 g_boost_clock[MAX_CORES] = {};

static volatile u64 g_boosts = 0;
static volatile u64 g_preemptions = 0;
static volatile u64 g_idle_wakeups = 0;
static volatile u64 g_steals = 0;
static volatile u64 g_ipis = 0;
static volatile u64 g_spawned = 0;
static volatile u64 g_exited = 0;
static volatile u32 g_next_id = 0;

static Satellite* g_sleepers = nullptr;
static cpu::Spinlock g_sleep_lock;
static volatile u64 g_next_wake = ~0ull;

static ExitRecord g_exits[EXIT_RING];
static u32 g_exit_next = 0;
static cpu::Spinlock g_exit_lock;
static WaitQueue g_exit_queue;

enum class Leave { Requeue, Die, Park };

static void copy_name(char* out, const char* name) {
    u32 i = 0;
    for (; name && name[i] && i + 1 < NAME_BYTES; ++i) out[i] = name[i];
    out[i] = 0;
}

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

static bool claim(Satellite* s, u32 epoch) {
    u32 seen = __atomic_load_n(&s->claimed_epoch, __ATOMIC_ACQUIRE);
    while (seen < epoch) {
        if (__atomic_compare_exchange_n(&s->claimed_epoch, &seen, epoch, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return true;
    }
    return false;
}

static void sleep_insert_locked(Satellite* s) {
    Satellite** link = &g_sleepers;
    while (*link && (*link)->wake_at <= s->wake_at) link = &(*link)->sleep_next;
    s->sleep_next = *link;
    *link = s;
    s->timed = true;
    __atomic_store_n(&g_next_wake, g_sleepers->wake_at, __ATOMIC_RELEASE);
}

static void sleep_remove_locked(Satellite* s) {
    if (!s->timed) return;
    for (Satellite** link = &g_sleepers; *link; link = &(*link)->sleep_next) {
        if (*link != s) continue;
        *link = s->sleep_next;
        break;
    }
    s->sleep_next = nullptr;
    s->timed = false;
    __atomic_store_n(&g_next_wake, g_sleepers ? g_sleepers->wake_at : ~0ull, __ATOMIC_RELEASE);
}

static u64 saturating_deadline(u64 now, u64 span) {
    return span > ~0ull - now ? ~0ull - 1 : now + span;
}

static bool any_stealable(u32 me) {
    u32 n = __atomic_load_n(&g_core_count, __ATOMIC_ACQUIRE);
    for (u32 i = 0; i < n; ++i) {
        u32 c = g_core_ids[i];
        if (c != me && !g_runq[c].empty()) return true;
    }
    return false;
}

static Satellite* try_steal(u32 me) {
    u32 n = __atomic_load_n(&g_core_count, __ATOMIC_ACQUIRE);
    if (n == 0) return nullptr;
    u32 start = __atomic_fetch_add(&g_rotor, 1u, __ATOMIC_RELAXED);
    for (u32 k = 0; k < n; ++k) {
        u32 victim = g_core_ids[(start + k) % n];
        if (victim == me || g_runq[victim].empty()) continue;
        g_lock[victim].lock();
        Satellite* s = g_runq[victim].pop_highest();
        g_lock[victim].unlock();
        if (s) {
            __atomic_fetch_add(&g_steals, 1ull, __ATOMIC_RELAXED);
            return s;
        }
    }
    return nullptr;
}

static u32 pick_idle_core(u32 me) {
    u32 n = __atomic_load_n(&g_core_count, __ATOMIC_ACQUIRE);
    if (n == 0) return ~0u;
    u32 start = __atomic_fetch_add(&g_rotor, 1u, __ATOMIC_RELAXED);
    for (u32 k = 0; k < n; ++k) {
        u32 c = g_core_ids[(start + k) % n];
        if (c != me && g_core_idle[c] && g_runq[c].empty()) return c;
    }
    return ~0u;
}

static void kick(u32 core) {
    __atomic_fetch_add(&g_ipis, 1ull, __ATOMIC_RELAXED);
    apic::send_ipi(core, idt::VEC_RESCHED);
}

static void place(Satellite* s, u32 me) {
    u32 target = pick_idle_core(me);
    if (target != ~0u) {
        g_lock[target].lock();
        g_runq[target].push(s);
        g_lock[target].unlock();
        kick(target);
        return;
    }
    g_lock[me].lock();
    g_runq[me].push(s);
    g_lock[me].unlock();
    Satellite* cur = g_current[me];
    if (cur && cur != &g_idle[me] && mlfq::preempts(s->ring, cur->ring)) {
        __atomic_fetch_add(&g_preemptions, 1ull, __ATOMIC_RELAXED);
        kick(me);
    }
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
    s->sleep_next = nullptr;
    s->waiting_on = nullptr;
    s->timed = false;
    s->has_deadline = false;
    s->id = 0;
    s->exit_status = 0;
    s->last_core = 0;
    s->run_ticks = 0;
    s->created_ms = clock::uptime_ms();
    s->blocked_ms = s->created_ms;
    fpu::init_state(s->fpu);
    s->name = name;
    s->entry = entry;
    s->arg = arg;
    mlfq::enter(*s, ring);
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
    if (self == &g_idle[apic::id()]) __atomic_add_fetch(&g_cores_on_kstack, 1u, __ATOMIC_SEQ_CST);
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
    next->last_core = apic::id();
}

static void reschedule_locked(u32 me, Leave how) {
    Satellite* old = g_current[me];
    Satellite* requeue = (how == Leave::Requeue && old != &g_idle[me]) ? old : nullptr;

    Satellite* next = g_runq[me].pop_highest();
    if (!next) {
        g_lock[me].unlock();
        next = try_steal(me);
        g_lock[me].lock();
    }
    if (!next) next = requeue ? requeue : &g_idle[me];

    if (requeue && next != requeue) g_runq[me].push(requeue);

    mlfq::dispatch(*next);

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
    u64 slept = (clock::uptime_ms() - s->blocked_ms) / clock::TICK_MS;
    mlfq::on_wake(*s, slept);
    s->state = State::Ready;
    place(s, me);
    cpu::irq_restore(f);
}

static void park_current(cpu::Spinlock* hold) {
    u32 me = apic::id();
    g_current[me]->blocked_ms = clock::uptime_ms();
    g_lock[me].lock();
    g_release_after[me] = hold;
    reschedule_locked(me, Leave::Park);
}

static bool zombies_pending() { return __atomic_load_n(&g_zombies, __ATOMIC_RELAXED) != nullptr; }

static void idle_entry(void*) {
    u32 me = apic::id();
    cpu::sti();
    for (;;) {
        reap_zombies();
        cpu::cli();
        g_core_idle[me] = 1;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (!g_runq[me].empty() || any_stealable(me) || zombies_pending()) {
            g_core_idle[me] = 0;
            g_lock[me].lock();
            reschedule_locked(me, Leave::Requeue);
            cpu::sti();
            asm volatile("pause" ::: "memory");
            continue;
        }
        cpu::sti_halt();
        g_core_idle[me] = 0;
    }
}

static void record_exit(Satellite* s) {
    u64 f = cpu::irq_save();
    g_exit_lock.lock();
    ExitRecord& r = g_exits[g_exit_next % EXIT_RING];
    ++g_exit_next;
    r.id = s->id;
    r.status = s->exit_status;
    r.run_ticks = s->run_ticks;
    copy_name(r.name, s->name);
    g_exit_lock.unlock();
    cpu::irq_restore(f);
    __atomic_fetch_add(&g_exited, 1ull, __ATOMIC_RELAXED);
    g_exit_queue.wake_all();
}

NORETURN void exit_with(i32 status) {
    cpu::cli();
    u32 me = apic::id();
    Satellite* cur = g_current[me];
    if (cur == &g_idle[me]) {
        cpu::sti();
        for (;;) cpu::halt();
    }

    cur->exit_status = status;
    record_exit(cur);

    g_lock[me].lock();
    reschedule_locked(me, Leave::Die);
    cpu::hang();
}

NORETURN void exit_current() { exit_with(0); }

static void wake_due_sleepers() {
    if (__atomic_load_n(&g_next_wake, __ATOMIC_ACQUIRE) > clock::uptime_ms()) return;
    struct Due { Satellite* s; u32 epoch; };
    Due due[MAX_SATELLITES];
    u32 n = 0;
    g_sleep_lock.lock();
    u64 now = clock::uptime_ms();
    while (g_sleepers && g_sleepers->wake_at <= now && n < MAX_SATELLITES) {
        Satellite* s = g_sleepers;
        g_sleepers = s->sleep_next;
        s->sleep_next = nullptr;
        s->timed = false;
        due[n++] = { s, s->epoch };
    }
    __atomic_store_n(&g_next_wake, g_sleepers ? g_sleepers->wake_at : ~0ull, __ATOMIC_RELEASE);
    g_sleep_lock.unlock();

    for (u32 i = 0; i < n; ++i) {
        Satellite* s = due[i].s;
        if (!claim(s, due[i].epoch)) continue;
        WaitQueue* q = s->waiting_on;
        if (q) q->detach(s);
        make_ready(s);
    }
}

static void timer_tick_handler(idt::Frame*) {
    apic::eoi();
    u32 me = apic::id();
    clock::tick(me);
    Satellite* cur = g_current[me];
    if (!cur) return;

    wake_due_sleepers();

    if (cur == &g_idle[me]) ++g_idle_ticks[me];
    else { ++g_busy_ticks[me]; ++cur->run_ticks; }

    if (++g_boost_clock[me] >= BOOST_TICKS) {
        g_boost_clock[me] = 0;
        g_lock[me].lock();
        g_runq[me].boost([](Satellite* s) { mlfq::on_boost(*s); });
        g_lock[me].unlock();
        if (cur != &g_idle[me]) mlfq::on_boost(*cur);
        __atomic_add_fetch(&g_boosts, 1, __ATOMIC_RELAXED);
    }

    if (cur == &g_idle[me]) {
        if (!g_runq[me].empty() || any_stealable(me)) { g_lock[me].lock(); reschedule_locked(me, Leave::Requeue); }
        return;
    }
    mlfq::Verdict v = mlfq::on_tick(*cur);
    if (v != mlfq::Verdict::Continue || g_runq[me].top_ring() < cur->ring) {
        g_lock[me].lock();
        reschedule_locked(me, Leave::Requeue);
    }
}

static void resched_ipi_handler(idt::Frame*) {
    apic::eoi();
    u32 me = apic::id();
    Satellite* cur = g_current[me];
    if (!cur) return;
    if (cur == &g_idle[me]) {
        if (!g_runq[me].empty() || any_stealable(me)) {
            __atomic_fetch_add(&g_idle_wakeups, 1ull, __ATOMIC_RELAXED);
            g_lock[me].lock();
            reschedule_locked(me, Leave::Requeue);
        }
        return;
    }
    if (g_runq[me].top_ring() < cur->ring) {
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

    bool cross_check = bootcfg::get().mode == bootcfg::Mode::Test;
    g_ticks_per_period = timesrc::calibrate(clock::TICK_MS, 4, cross_check).apic_ticks;
    serial::printf("[orbital] APIC timer: %u ticks per %ums (divide/16)\n",
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
    idt::set_handler(idt::VEC_RESCHED, &resched_ipi_handler);
    u32 me = apic::id();
    if (me >= MAX_CORES) {
        serial::printf("[orbital] FATAL: local APIC id %u is beyond the %u cores the scheduler tracks\n", me, MAX_CORES);
        cpu::hang();
    }
    build_satellite(&g_idle[me], "idle", &idle_entry, nullptr, NUM_RINGS - 1, &satellite_trampoline);
    apic::start_timer_periodic(idt::VEC_APIC_TIMER, g_ticks_per_period, 4);

    u32 n = __atomic_load_n(&g_core_count, __ATOMIC_ACQUIRE);
    bool known = false;
    for (u32 i = 0; i < n; ++i) known = known || g_core_ids[i] == me;
    if (!known) {
        u32 slot = __atomic_fetch_add(&g_core_count, 1u, __ATOMIC_ACQ_REL);
        g_core_ids[slot] = me;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }
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
    s->id = __atomic_add_fetch(&g_next_id, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_spawned, 1ull, __ATOMIC_RELAXED);
    u64 f = cpu::irq_save();
    place(s, apic::id());
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
    mlfq::pin_lowest(*cur);
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

u32 current_id() {
    Satellite* cur = current();
    return cur ? cur->id : 0;
}

int current_ring() {
    Satellite* cur = current();
    return cur ? cur->ring : -1;
}

u64 boosts() { return __atomic_load_n(&g_boosts, __ATOMIC_RELAXED); }

u32 snapshot(SatelliteInfo* out, u32 max) {
    u64 now = clock::uptime_ms();
    u32 n = 0;
    for (auto& s : g_pool) {
        if (n >= max) break;
        if (!__atomic_load_n(&s.in_use, __ATOMIC_ACQUIRE) || s.id == 0) continue;
        SatelliteInfo& i = out[n++];
        i.id = s.id;
        i.state = s.state;
        i.ring = s.ring;
        i.core = s.last_core;
        i.switches = s.switches;
        i.run_ticks = s.run_ticks;
        i.age_ms = now - s.created_ms;
        copy_name(i.name, s.name);
    }
    return n;
}

u32 cores(CoreInfo* out, u32 max) {
    u32 count = __atomic_load_n(&g_core_count, __ATOMIC_ACQUIRE);
    u32 n = 0;
    for (u32 k = 0; k < count && n < max; ++k) {
        u32 c = g_core_ids[k];
        CoreInfo& i = out[n++];
        i.apic_id = c;
        i.idle = g_core_idle[c] != 0;
        i.queued = g_runq[c].size();
        i.busy_ticks = g_busy_ticks[c];
        i.idle_ticks = g_idle_ticks[c];
        Satellite* cur = g_current[c];
        copy_name(i.running, cur ? cur->name : "-");
    }
    return n;
}

Stats stats() {
    Stats s;
    s.boosts = __atomic_load_n(&g_boosts, __ATOMIC_RELAXED);
    s.preemptions = __atomic_load_n(&g_preemptions, __ATOMIC_RELAXED);
    s.idle_wakeups = __atomic_load_n(&g_idle_wakeups, __ATOMIC_RELAXED);
    s.steals = __atomic_load_n(&g_steals, __ATOMIC_RELAXED);
    s.ipis_sent = __atomic_load_n(&g_ipis, __ATOMIC_RELAXED);
    s.spawned = __atomic_load_n(&g_spawned, __ATOMIC_RELAXED);
    s.exited = __atomic_load_n(&g_exited, __ATOMIC_RELAXED);
    return s;
}

void sleep_ms(u32 ms) {
    u64 f = cpu::irq_save();
    u32 me = apic::id();
    Satellite* cur = g_current[me];
    if (!cur || cur == &g_idle[me] || ms == 0) {
        cpu::irq_restore(f);
        if (ms == 0) yield();
        return;
    }
    g_sleep_lock.lock();
    ++cur->epoch;
    cur->waiting_on = nullptr;
    cur->has_deadline = true;
    cur->wake_at = clock::uptime_ms() + ms + clock::TICK_MS;
    cur->state = State::Sleeping;
    sleep_insert_locked(cur);
    park_current(&g_sleep_lock);
    cpu::irq_restore(f);
}

void WaitQueue::wait_until(bool (*ready)(void*), void* arg) {
    wait_until_for(ready, arg, FOREVER);
}

bool WaitQueue::wait_until_for(bool (*ready)(void*), void* arg, u64 timeout_ms) {
    const bool forever = timeout_ms == FOREVER;
    const u64 deadline = forever ? 0 : saturating_deadline(clock::uptime_ms(), timeout_ms);
    for (;;) {
        u64 f = cpu::irq_save();
        lock_.lock();
        if (ready(arg)) {
            lock_.unlock();
            cpu::irq_restore(f);
            return true;
        }
        if (!forever && clock::uptime_ms() >= deadline) {
            lock_.unlock();
            cpu::irq_restore(f);
            return false;
        }
        u32 me = apic::id();
        Satellite* cur = g_current[me];
        if (!cur || cur == &g_idle[me]) {
            lock_.unlock();
            cpu::irq_restore(f);
            asm volatile("pause" ::: "memory");
            continue;
        }
        ++cur->epoch;
        cur->state = State::Blocked;
        cur->wait_next = nullptr;
        cur->waiting_on = this;
        cur->has_deadline = !forever;
        if (tail_) tail_->wait_next = cur; else head_ = cur;
        tail_ = cur;
        if (!forever) {
            g_sleep_lock.lock();
            cur->wake_at = deadline;
            sleep_insert_locked(cur);
            g_sleep_lock.unlock();
        }
        park_current(&lock_);
        cpu::irq_restore(f);
    }
}

bool WaitQueue::detach(Satellite* s) {
    u64 f = cpu::irq_save();
    lock_.lock();
    bool found = false;
    Satellite* prev = nullptr;
    for (Satellite* it = head_; it; prev = it, it = it->wait_next) {
        if (it != s) continue;
        if (prev) prev->wait_next = it->wait_next;
        else head_ = it->wait_next;
        if (tail_ == it) tail_ = prev;
        it->wait_next = nullptr;
        found = true;
        break;
    }
    lock_.unlock();
    cpu::irq_restore(f);
    return found;
}

static void finish_queue_wake(Satellite* s) {
    s->waiting_on = nullptr;
    if (s->has_deadline) {
        u64 f = cpu::irq_save();
        g_sleep_lock.lock();
        sleep_remove_locked(s);
        g_sleep_lock.unlock();
        cpu::irq_restore(f);
    }
    make_ready(s);
}

u32 WaitQueue::wake_one() {
    u64 f = cpu::irq_save();
    lock_.lock();
    Satellite* won = nullptr;
    Satellite* prev = nullptr;
    for (Satellite* it = head_; it; prev = it, it = it->wait_next) {
        if (!claim(it, it->epoch)) continue;
        if (prev) prev->wait_next = it->wait_next;
        else head_ = it->wait_next;
        if (tail_ == it) tail_ = prev;
        it->wait_next = nullptr;
        won = it;
        break;
    }
    lock_.unlock();
    cpu::irq_restore(f);
    if (won) finish_queue_wake(won);
    return won ? 1 : 0;
}

u32 WaitQueue::wake_all() {
    u64 f = cpu::irq_save();
    lock_.lock();
    Satellite* won = nullptr;
    Satellite* keep_head = nullptr;
    Satellite* keep_tail = nullptr;
    Satellite* it = head_;
    while (it) {
        Satellite* following = it->wait_next;
        it->wait_next = nullptr;
        if (claim(it, it->epoch)) {
            it->next = won;
            won = it;
        } else {
            if (keep_tail) keep_tail->wait_next = it; else keep_head = it;
            keep_tail = it;
        }
        it = following;
    }
    head_ = keep_head;
    tail_ = keep_tail;
    lock_.unlock();
    cpu::irq_restore(f);

    u32 n = 0;
    while (won) {
        Satellite* following = won->next;
        won->next = nullptr;
        finish_queue_wake(won);
        won = following;
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

bool Semaphore::positive(void* self) {
    return __atomic_load_n(&static_cast<Semaphore*>(self)->count_, __ATOMIC_ACQUIRE) > 0;
}

bool Semaphore::try_acquire() {
    u32 c = __atomic_load_n(&count_, __ATOMIC_RELAXED);
    while (c > 0) {
        if (__atomic_compare_exchange_n(&count_, &c, c - 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return true;
    }
    return false;
}

void Semaphore::acquire() {
    while (!try_acquire()) queue_.wait_until(&positive, this);
}

bool Semaphore::acquire_for(u64 timeout_ms) {
    if (try_acquire()) return true;
    const u64 deadline = saturating_deadline(clock::uptime_ms(), timeout_ms);
    for (;;) {
        u64 now = clock::uptime_ms();
        if (now >= deadline) return try_acquire();
        queue_.wait_until_for(&positive, this, deadline - now);
        if (try_acquire()) return true;
    }
}

void Semaphore::release(u32 n) {
    __atomic_add_fetch(&count_, n, __ATOMIC_RELEASE);
    for (u32 i = 0; i < n; ++i)
        if (queue_.wake_one() == 0) break;
}

bool SleepLock::held_by_me() const { return __atomic_load_n(&owner_, __ATOMIC_ACQUIRE) == self_token(); }

bool SleepLock::is_free(void* self) {
    return __atomic_load_n(&static_cast<SleepLock*>(self)->owner_, __ATOMIC_ACQUIRE) == 0;
}

bool SleepLock::take(u64 me) {
    u64 expected = 0;
    return __atomic_compare_exchange_n(&owner_, &expected, me, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

bool SleepLock::try_lock() { return take(self_token()); }

void SleepLock::lock() {
    u64 me = self_token();
    if (__atomic_load_n(&owner_, __ATOMIC_ACQUIRE) == me) {
        serial::writeln("[orbital] FATAL: SleepLock is not recursive and was locked twice by the same Satellite");
        cpu::hang();
    }
    while (!take(me)) queue_.wait_until(&is_free, this);
}

bool SleepLock::lock_for(u64 timeout_ms) {
    u64 me = self_token();
    if (__atomic_load_n(&owner_, __ATOMIC_ACQUIRE) == me) {
        serial::writeln("[orbital] FATAL: SleepLock is not recursive and was locked twice by the same Satellite");
        cpu::hang();
    }
    const u64 deadline = saturating_deadline(clock::uptime_ms(), timeout_ms);
    for (;;) {
        if (take(me)) return true;
        u64 now = clock::uptime_ms();
        if (now >= deadline) return take(me);
        queue_.wait_until_for(&is_free, this, deadline - now);
    }
}

void SleepLock::unlock() {
    if (!held_by_me()) {
        serial::writeln("[orbital] FATAL: SleepLock unlocked by a Satellite that does not hold it");
        cpu::hang();
    }
    __atomic_store_n(&owner_, 0, __ATOMIC_RELEASE);
    queue_.wake_one();
}

namespace {

struct JoinWait {
    u32 id;
    JoinResult result;
    i32 status;
};

bool join_ready(void* p) {
    auto* w = static_cast<JoinWait*>(p);
    u64 f = cpu::irq_save();
    g_exit_lock.lock();
    bool found = false;
    for (u32 i = 0; i < EXIT_RING; ++i) {
        const ExitRecord& r = g_exits[i];
        if (r.id == w->id && r.id != 0) { w->status = r.status; found = true; break; }
    }
    g_exit_lock.unlock();
    cpu::irq_restore(f);
    if (found) { w->result = JoinResult::Exited; return true; }
    for (auto& s : g_pool) {
        if (__atomic_load_n(&s.in_use, __ATOMIC_ACQUIRE) && s.id == w->id) return false;
    }
    w->result = JoinResult::Unknown;
    return true;
}

}

JoinResult join(u32 id, i32* status, u64 timeout_ms) {
    if (id == 0) return JoinResult::Unknown;
    if (id == current_id()) return JoinResult::Self;
    JoinWait w{ id, JoinResult::TimedOut, 0 };
    bool done = g_exit_queue.wait_until_for(&join_ready, &w, timeout_ms);
    if (!done) return JoinResult::TimedOut;
    if (w.result == JoinResult::Exited && status) *status = w.status;
    return w.result;
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
