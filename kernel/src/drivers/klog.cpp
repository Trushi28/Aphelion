#include <cosmos/klog.hpp>
#include <cosmos/cpu.hpp>

namespace klog {

namespace {

Ring<CAPACITY> g_ring;
cpu::Spinlock g_lock;

bool try_lock() { return !__atomic_exchange_n(&g_lock.locked, 1, __ATOMIC_ACQUIRE); }

}

void put(char c) {
    u64 flags = cpu::irq_save();
    if (try_lock()) {
        g_ring.put(c);
        g_lock.unlock();
    }
    cpu::irq_restore(flags);
}

u64 tail() { return g_ring.tail_seq(); }

u32 read(u64& seq, char* out, u32 max) {
    u64 flags = cpu::irq_save();
    g_lock.lock();
    u32 n = g_ring.read(seq, out, max);
    g_lock.unlock();
    cpu::irq_restore(flags);
    return n;
}

u64 dropped(u64 from) {
    u64 flags = cpu::irq_save();
    g_lock.lock();
    u64 d = g_ring.dropped(from);
    g_lock.unlock();
    cpu::irq_restore(flags);
    return d;
}

}
