#include <cosmos/dma.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/serial.hpp>

namespace dma {

constexpr u32 MAX_LIVE = 256;
struct Live { u64 virt, phys, bytes; bool used; };
static Live g_live[MAX_LIVE];
static cpu::Spinlock g_lock;

struct Guard {
    u64 flags;
    Guard() : flags(cpu::irq_save()) { g_lock.lock(); }
    ~Guard() { g_lock.unlock(); cpu::irq_restore(flags); }
};

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

    bool stored = false;
    {
        Guard g;
        for (auto& l : g_live) {
            if (l.used) continue;
            l = { virt, phys, size, true };
            stored = true;
            break;
        }
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
        for (auto& l : g_live)
            if (l.used && l.virt == reinterpret_cast<u64>(b.virt)) { l.used = false; found = true; break; }
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
        for (const auto& l : g_live) {
            if (l.used && v >= l.virt && len <= l.bytes && v - l.virt <= l.bytes - len) {
                r = l.phys + (v - l.virt);
                break;
            }
        }
    }
    if (!r) {
        serial::printf("[dma] FATAL: %p (+%lu) is not inside a DMA buffer, refusing to give it to a device\n", v, len);
        cpu::hang();
    }
    return r;
}

}