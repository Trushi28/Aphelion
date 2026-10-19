#include <cosmos/input.hpp>
#include <cosmos/orbital.hpp>
#include <cosmos/keyboard.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/clock.hpp>
#include <cosmos/cpu.hpp>

namespace input {

namespace {

constexpr u32 CAPACITY = 256;
constexpr u64 POLL_SLICE_MS = 25;
constexpr u64 ESC_TIMEOUT_MS = 40;

Key g_ring[CAPACITY];
u32 g_head = 0;
u32 g_tail = 0;
u64 g_dropped = 0;
u64 g_total = 0;
cpu::Spinlock g_lock;
orbital::WaitQueue g_wait;
bool g_ready = false;

Set1Decoder g_scan;
AnsiDecoder g_ansi;
cpu::Spinlock g_scan_lock;
cpu::Spinlock g_ansi_lock;
u64 g_ansi_last_ms = 0;

bool has_data(void*) {
    return __atomic_load_n(&g_head, __ATOMIC_ACQUIRE) != __atomic_load_n(&g_tail, __ATOMIC_ACQUIRE);
}

bool pop(Key& out) {
    u64 flags = cpu::irq_save();
    g_lock.lock();
    bool ok = g_head != g_tail;
    if (ok) {
        out = g_ring[g_tail % CAPACITY];
        __atomic_store_n(&g_tail, g_tail + 1, __ATOMIC_RELEASE);
    }
    g_lock.unlock();
    cpu::irq_restore(flags);
    return ok;
}

}

void init() {
    g_head = g_tail = 0;
    g_dropped = g_total = 0;
    g_ready = true;
}

bool ready() { return g_ready; }

bool push(const Key& key) {
    if (!g_ready) return false;
    u64 flags = cpu::irq_save();
    g_lock.lock();
    bool ok = g_head - g_tail < CAPACITY;
    if (ok) {
        g_ring[g_head % CAPACITY] = key;
        __atomic_store_n(&g_head, g_head + 1, __ATOMIC_RELEASE);
        ++g_total;
    } else {
        ++g_dropped;
    }
    g_lock.unlock();
    cpu::irq_restore(flags);
    if (ok) g_wait.wake_one();
    return ok;
}

void feed_scancode(u8 byte) {
    Key k;
    u64 flags = cpu::irq_save();
    g_scan_lock.lock();
    bool got = g_scan.feed(byte, &k);
    g_scan_lock.unlock();
    cpu::irq_restore(flags);
    if (got) push(k);
}

void feed_serial(u8 byte) {
    Key out[2];
    u64 flags = cpu::irq_save();
    g_ansi_lock.lock();
    u32 n = g_ansi.feed(byte, out);
    g_ansi_last_ms = g_ansi.pending() ? clock::now_ms() : 0;
    g_ansi_lock.unlock();
    cpu::irq_restore(flags);
    for (u32 i = 0; i < n; ++i) push(out[i]);
}

bool try_read(Key& out) { return pop(out); }

void pump() {
    keyboard::poll();
    serial::poll_rx();
    u64 flags = cpu::irq_save();
    g_ansi_lock.lock();
    Key out[2];
    u32 n = 0;
    if (g_ansi.pending() && g_ansi_last_ms && clock::now_ms() - g_ansi_last_ms >= ESC_TIMEOUT_MS) {
        n = g_ansi.idle(out);
        g_ansi_last_ms = 0;
    }
    g_ansi_lock.unlock();
    cpu::irq_restore(flags);
    for (u32 i = 0; i < n; ++i) push(out[i]);
}

bool read_for(Key& out, u64 timeout_ms) {
    u64 start = clock::now_ms();
    for (;;) {
        pump();
        if (pop(out)) return true;
        u64 elapsed = clock::now_ms() - start;
        if (elapsed >= timeout_ms) return false;
        u64 left = timeout_ms - elapsed;
        g_wait.wait_until_for(&has_data, nullptr, left < POLL_SLICE_MS ? left : POLL_SLICE_MS);
    }
}

void read(Key& out) {
    for (;;) {
        pump();
        if (pop(out)) return;
        g_wait.wait_until_for(&has_data, nullptr, POLL_SLICE_MS);
    }
}

u32 pending() {
    u32 h = __atomic_load_n(&g_head, __ATOMIC_ACQUIRE);
    u32 t = __atomic_load_n(&g_tail, __ATOMIC_ACQUIRE);
    return h - t;
}

u64 dropped() { return g_dropped; }
u64 total() { return g_total; }

void flush() {
    Key k;
    while (pop(k)) {}
}

bool selftest() {
    flush();
    bool ok = true;
    u64 total_before = g_total;
    feed_scancode(0x1E);
    feed_scancode(0x9E);
    feed_scancode(0x2A);
    feed_scancode(0x23);
    feed_scancode(0xAA);
    Key k;
    ok = ok && try_read(k) && k == text_key('a');
    ok = ok && try_read(k) && k == text_key('H', MOD_SHIFT);
    ok = ok && !try_read(k);
    const char seq[] = "\x1b[A\xE2\x82\xB9";
    for (u32 i = 0; seq[i]; ++i) feed_serial(static_cast<u8>(seq[i]));
    ok = ok && try_read(k) && k == sym_key(Sym::Up);
    ok = ok && try_read(k) && k == text_key(0x20B9);
    ok = ok && !try_read(k);
    ok = ok && g_total == total_before + 4;
    for (u32 i = 0; i < CAPACITY + 8; ++i) push(text_key('x'));
    ok = ok && pending() == CAPACITY && g_dropped >= 8;
    flush();
    ok = ok && pending() == 0;
    g_dropped = 0;
    return ok;
}

}
