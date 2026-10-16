#include <cosmos/dma.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/serial.hpp>

namespace dma {

constexpr u32 MAX_LIVE = 256;
struct Live { u64 virt, phys, bytes; };
static Live g_live[MAX_LIVE];
static u32 g_count = 0;
static cpu::Spinlock g_lock;

struct Guard {
    u64 flags;
    Guard() : flags(cpu::irq_save()) { g_lock.lock(); }
    ~Guard() { g_lock.unlock(); cpu::irq_restore(flags); }
};

static u32 lower_bound_locked(u64 virt) {
    u32 lo = 0, hi = g_count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (g_live[mid].virt < virt) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static bool insert_locked(const Live& l) {
    if (g_count == MAX_LIVE) return false;
    u32 at = lower_bound_locked(l.virt);
    for (u32 i = g_count; i > at; --i) g_live[i] = g_live[i - 1];
    g_live[at] = l;
    ++g_count;
    return true;
}

static bool remove_locked(u64 virt) {
    u32 at = lower_bound_locked(virt);
    if (at == g_count || g_live[at].virt != virt) return false;
    for (u32 i = at; i + 1 < g_count; ++i) g_live[i] = g_live[i + 1];
    --g_count;
    return true;
}

Buffer alloc(u64 bytes) {
    Buffer b;
    if (bytes == 0) return b;
    int order = 0;
    while (order <= universe::MAX_ORDER && (universe::PAGE_SIZE << order) < bytes) ++order;
    if (order > universe::MAX_ORDER) return b;

    u64 phys = universe::alloc_zeroed(order, true);
    if (!phys) return b;
    u64 size = universe::PAGE_SIZE << order;
    u64 virt = reinterpret_cast<u64>(universe::to_virt(phys));

    bool stored;
    {
        Guard g;
        stored = insert_locked({ virt, phys, size });
    }
    if (!stored) {
        universe::free(phys, order);
        serial::writeln("[dma] buffer table full");
        return b;
    }
    b.virt = reinterpret_cast<void*>(virt);
    b.phys = phys;
    b.bytes = size;
    b.order = order;
    return b;
}

void release(Buffer& b) {
    if (!b.ok()) return;
    bool found = false;
    {
        Guard g;
        found = remove_locked(reinterpret_cast<u64>(b.virt));
    }
    if (!found) {
        serial::printf("[dma] FATAL: release of a buffer that is not live (%p)\n", reinterpret_cast<u64>(b.virt));
        cpu::hang();
    }
    universe::free(b.phys, b.order);
    b = Buffer{};
}

u64 phys(const void* p, u64 len) {
    u64 v = reinterpret_cast<u64>(p);
    u64 r = 0;
    {
        Guard g;
        u32 at = lower_bound_locked(v);
        if (at < g_count && g_live[at].virt == v) ++at;
        if (at > 0) {
            const Live& l = g_live[at - 1];
            if (v >= l.virt && len <= l.bytes && v - l.virt <= l.bytes - len) r = l.phys + (v - l.virt);
        }
    }
    if (!r) {
        serial::printf("[dma] FATAL: %p (+%lu) is not inside a DMA buffer, refusing to give it to a device\n", v, len);
        cpu::hang();
    }
    return r;
}

}