#include <cosmos/tlb.hpp>
#include <cosmos/apic.hpp>
#include <cosmos/idt.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/serial.hpp>

namespace tlb {

constexpr u64 PAGE = 0x1000;
constexpr u64 INVLPG_LIMIT = 32 * PAGE;
constexpr u64 SPIN_LIMIT = 1ull << 31;

enum Kind : u32 { FLUSH = 0, PROBE = 1 };

struct Request {
    u32 kind;
    u64 addr;
    u64 bytes;
    u64* probe_out;
    u32 probe_slots;
};

static u32 g_apic_id[MAX_CORES];
static volatile u32 g_registered = 0;
static volatile u64 g_online = 0;
static volatile u64 g_pending = 0;
static volatile int g_req_lock = 0;
static Request g_req;

static void flush_local(u64 addr, u64 bytes) {
    if (bytes == ALL_ADDRESSES || bytes > INVLPG_LIMIT) {
        cpu::write_cr3(cpu::read_cr3());
        return;
    }
    u64 start = addr & ~(PAGE - 1);
    u64 end = (addr + bytes + PAGE - 1) & ~(PAGE - 1);
    for (u64 a = start; a < end; a += PAGE) cpu::invlpg(a);
}

static u32 index_of(u32 apic_id) {
    u32 n = __atomic_load_n(&g_registered, __ATOMIC_ACQUIRE);
    for (u32 i = 0; i < n; ++i)
        if (g_apic_id[i] == apic_id) return i;
    return ~0u;
}

u32 core_index() {
    if (!__atomic_load_n(&g_registered, __ATOMIC_ACQUIRE)) return ~0u;
    return index_of(apic::id());
}
u64 online_mask() { return __atomic_load_n(&g_online, __ATOMIC_ACQUIRE); }

u32 online_count() {
    u64 m = online_mask();
    u32 n = 0;
    while (m) { n += static_cast<u32>(m & 1); m >>= 1; }
    return n;
}

static void serve(u32 me) {
    u64 bit = 1ull << me;
    if (!(__atomic_load_n(&g_pending, __ATOMIC_ACQUIRE) & bit)) return;
    if (g_req.kind == PROBE) {
        u64 v = cpu::read_u64(g_req.addr);
        if (me < g_req.probe_slots) g_req.probe_out[me] = v;
    } else {
        flush_local(g_req.addr, g_req.bytes);
    }
    __atomic_and_fetch(&g_pending, ~bit, __ATOMIC_RELEASE);
}

void poll() {
    u32 me = core_index();
    if (me != ~0u) serve(me);
}

static void ipi_handler(idt::Frame*) {
    u32 me = core_index();
    if (me != ~0u) serve(me);
    apic::eoi();
}

void init_core() {
    u32 id = apic::id();
    u32 slot = __atomic_fetch_add(&g_registered, 1u, __ATOMIC_ACQ_REL);
    if (slot >= MAX_CORES) {
        serial::writeln("[tlb] FATAL: more cores than the shootdown mask can hold");
        cpu::hang();
    }
    g_apic_id[slot] = id;
    if (slot == 0) idt::set_handler(idt::VEC_TLB_SHOOTDOWN, &ipi_handler);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    cpu::write_cr3(cpu::read_cr3());
    __atomic_or_fetch(&g_online, 1ull << slot, __ATOMIC_SEQ_CST);
}

static void lock_request() {
    for (;;) {
        if (!__atomic_exchange_n(&g_req_lock, 1, __ATOMIC_ACQUIRE)) return;
        while (__atomic_load_n(&g_req_lock, __ATOMIC_RELAXED)) {
            poll();
            asm volatile("pause" ::: "memory");
        }
    }
}

static void unlock_request() { __atomic_store_n(&g_req_lock, 0, __ATOMIC_RELEASE); }

static void broadcast(u64 remote) {
    __atomic_store_n(&g_pending, remote, __ATOMIC_SEQ_CST);
    u32 n = __atomic_load_n(&g_registered, __ATOMIC_ACQUIRE);
    for (u32 i = 0; i < n; ++i)
        if (remote & (1ull << i)) apic::send_ipi(g_apic_id[i], idt::VEC_TLB_SHOOTDOWN);
    u64 spins = 0;
    while (__atomic_load_n(&g_pending, __ATOMIC_ACQUIRE)) {
        poll();
        asm volatile("pause" ::: "memory");
        if (++spins > SPIN_LIMIT) {
            serial::printf("[tlb] FATAL: shootdown not acknowledged, still pending mask %lx\n",
                            __atomic_load_n(&g_pending, __ATOMIC_ACQUIRE));
            cpu::hang();
        }
    }
}

void flush(u64 core_mask, u64 addr, u64 bytes) {
    u64 flags = cpu::irq_save();
    flush_local(addr, bytes);
    u32 me = core_index();
    u64 remote = core_mask & online_mask();
    if (me != ~0u) remote &= ~(1ull << me);
    if (remote) {
        lock_request();
        g_req = { FLUSH, addr, bytes, nullptr, 0 };
        broadcast(remote);
        unlock_request();
    }
    cpu::irq_restore(flags);
}

bool selftest_probe(u64 addr, u64* out, u32 slots) {
    u64 flags = cpu::irq_save();
    u32 me = core_index();
    if (me == ~0u || me >= slots) { cpu::irq_restore(flags); return false; }
    out[me] = cpu::read_u64(addr);
    u64 remote = online_mask() & ~(1ull << me);
    if (remote) {
        lock_request();
        g_req = { PROBE, addr, PAGE, out, slots };
        broadcast(remote);
        unlock_request();
    }
    cpu::irq_restore(flags);
    return true;
}

}
