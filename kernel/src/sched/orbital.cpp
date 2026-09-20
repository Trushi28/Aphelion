#include <cosmos/orbital.hpp>
#include <cosmos/apic.hpp>
#include <cosmos/pit.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/serial.hpp>

extern "C" void switch_context(u64* old_rsp_out, u64 new_rsp);

namespace orbital {

constexpr int MAX_SATELLITES = 32;
constexpr int STACK_ORDER = 2;

static u64 g_hhdm = 0;
static u32 g_ticks_per_period = 0;

static Satellite g_pool[MAX_SATELLITES];
static Satellite g_idle[256];
static Satellite* g_current[256] = {};

static Satellite* g_ring_head[NUM_RINGS] = {};
static Satellite* g_ring_tail[NUM_RINGS] = {};
static cpu::Spinlock g_lock;

static u32 ticks_for_ring(int ring) {

    static const u32 slice[NUM_RINGS] = {2, 4, 8, 16};
    return slice[ring < NUM_RINGS ? ring : NUM_RINGS - 1];
}

static void enqueue_locked(Satellite* s) {
    s->next = nullptr;
    int r = s->ring;
    if (!g_ring_head[r]) g_ring_head[r] = g_ring_tail[r] = s;
    else { g_ring_tail[r]->next = s; g_ring_tail[r] = s; }
}
static Satellite* dequeue_highest_locked() {
    for (int r = 0; r < NUM_RINGS; ++r) {
        if (g_ring_head[r]) {
            Satellite* s = g_ring_head[r];
            g_ring_head[r] = s->next;
            if (!g_ring_head[r]) g_ring_tail[r] = nullptr;
            return s;
        }
    }
    return nullptr;
}
static bool any_ready() {
    for (int r = 0; r < NUM_RINGS; ++r) if (g_ring_head[r]) return true;
    return false;
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

    g_lock.unlock();
    Satellite* self = g_current[apic::id()];
    cpu::sti();
    self->entry(self->arg);

    for (;;) yield();
}

static void idle_entry(void*) {
    cpu::sti();
    for (;;) cpu::halt();
}

static void reschedule_locked(Satellite* old_to_enqueue) {
    if (old_to_enqueue) enqueue_locked(old_to_enqueue);

    u32 me = apic::id();
    Satellite* next = dequeue_highest_locked();
    if (!next) next = &g_idle[me];

    next->ticks_left = ticks_for_ring(next->ring);

    Satellite* old = g_current[me];
    if (next == old) { g_lock.unlock(); return; }

    g_current[me] = next;
    switch_context(&old->rsp, next->rsp);
    g_lock.unlock();
}

static void timer_tick_handler(idt::Frame*) {
    apic::eoi();
    u32 me = apic::id();
    Satellite* cur = g_current[me];
    if (!cur) return;

    if (cur == &g_idle[me]) {
        if (any_ready()) { g_lock.lock(); reschedule_locked(nullptr); }
    } else if (cur->ticks_left > 0 && --cur->ticks_left == 0) {
        if (cur->ring < NUM_RINGS - 1) ++cur->ring;
        g_lock.lock();
        reschedule_locked(cur);
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
    g_lock.lock();
    Satellite* s = nullptr;
    for (auto& cand : g_pool) if (!cand.in_use) { s = &cand; break; }
    if (!s) { g_lock.unlock(); return nullptr; }
    s->in_use = true;
    g_lock.unlock();

    build_satellite(s, name, entry, arg, 0, &satellite_trampoline);
    g_lock.lock(); enqueue_locked(s); g_lock.unlock();
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
    g_lock.lock();
    reschedule_locked(cur);
    cpu::sti();
}

NORETURN void start_core() {
    u32 me = apic::id();
    g_current[me] = &g_idle[me];
    u64 scratch_rsp;
    switch_context(&scratch_rsp, g_idle[me].rsp);
    cpu::hang();
}

}
