#include <cosmos/serial.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/klog.hpp>
#include <cosmos/input.hpp>
#include <cosmos/idt.hpp>
#include <cosmos/apic.hpp>

namespace serial {

static constexpr u16 COM1 = 0x3F8;
static constexpr u32 TX_SPINS = 4000000;
static constexpr u32 TX_STUCK_SPINS = 2000;
static constexpr u32 TX_LOCK_SPINS = 50000000;
static constexpr u32 RX_DRAIN_LIMIT = 64;
static constexpr u32 ISR_ROUNDS = 8;

static bool g_present = false;
static bool g_rx_irq = false;
static u64 g_rx_bytes = 0;
static u64 g_tx_drops = 0;
static volatile bool g_tx_stuck = false;
static cpu::Spinlock g_rx_lock;

void init() {
    cpu::out8(COM1 + 7, 0xA5);
    u8 a = cpu::in8(COM1 + 7);
    cpu::out8(COM1 + 7, 0x5A);
    u8 b = cpu::in8(COM1 + 7);
    g_present = a == 0xA5 && b == 0x5A && cpu::in8(COM1 + 5) != 0xFF;
    if (!g_present) return;

    cpu::out8(COM1 + 1, 0x00);
    cpu::out8(COM1 + 3, 0x80);
    cpu::out8(COM1 + 0, 0x01);
    cpu::out8(COM1 + 1, 0x00);
    cpu::out8(COM1 + 3, 0x03);
    cpu::out8(COM1 + 2, 0xC7);
    cpu::out8(COM1 + 4, 0x0B);
    for (u32 i = 0; i < RX_DRAIN_LIMIT && (cpu::in8(COM1 + 5) & 0x01); ++i) cpu::in8(COM1);
}

bool present() { return g_present; }
bool rx_irq_enabled() { return g_rx_irq; }
u64 rx_bytes() { return g_rx_bytes; }
u64 tx_drops() { return g_tx_drops; }

static bool wait_tx() {
    u32 limit = g_tx_stuck ? TX_STUCK_SPINS : TX_SPINS;
    for (u32 spins = 0; spins < limit; ++spins) {
        if (cpu::in8(COM1 + 5) & 0x20) {
            g_tx_stuck = false;
            return true;
        }
        cpu::io_wait();
    }
    g_tx_stuck = true;
    ++g_tx_drops;
    return false;
}

static void emit(char c) {
    if (!g_present) return;
    if (c == '\n') {
        if (!wait_tx()) return;
        cpu::out8(COM1, '\r');
    }
    if (!wait_tx()) return;
    cpu::out8(COM1, static_cast<u8>(c));
}

static cpu::Spinlock g_tx_lock;

struct TxGuard {
    u64 flags;
    bool held;
    TxGuard() {
        flags = cpu::irq_save();
        held = false;
        for (u32 spins = 0; spins < TX_LOCK_SPINS; ++spins) {
            if (!__atomic_exchange_n(&g_tx_lock.locked, 1, __ATOMIC_ACQUIRE)) {
                held = true;
                break;
            }
            asm volatile("pause");
        }
    }
    ~TxGuard() {
        if (held) g_tx_lock.unlock();
        cpu::irq_restore(flags);
    }
};

static void putc_raw(char c) {
    klog::put(c);
    emit(c);
}

static void write_raw(const char* s) { while (*s) putc_raw(*s++); }

void putc(char c) {
    TxGuard guard;
    putc_raw(c);
}

void console_write(const char* s, u32 n) {
    TxGuard guard;
    for (u32 i = 0; i < n; ++i) emit(s[i]);
}

void write(const char* s) {
    TxGuard guard;
    write_raw(s);
}

void writeln(const char* s) {
    TxGuard guard;
    write_raw(s);
    putc_raw('\n');
}

static void drain_rx() {
    for (u32 n = 0; n < RX_DRAIN_LIMIT; ++n) {
        u8 lsr = cpu::in8(COM1 + 5);
        if (lsr == 0xFF || !(lsr & 0x01)) return;
        u8 byte = cpu::in8(COM1);
        if (lsr & 0x0E) continue;
        ++g_rx_bytes;
        input::feed_serial(byte);
    }
}

static void rx_isr(idt::Frame*) {
    g_rx_lock.lock();
    for (u32 i = 0; i < ISR_ROUNDS; ++i) {
        u8 iir = cpu::in8(COM1 + 2);
        if (iir & 0x01) break;
        cpu::in8(COM1 + 6);
        drain_rx();
    }
    g_rx_lock.unlock();
    apic::eoi();
}

bool enable_rx_irq(u8 vector) {
    if (!g_present) return false;
    idt::set_handler(vector, &rx_isr);
    cpu::out8(COM1 + 4, 0x0B);
    cpu::out8(COM1 + 1, 0x01);
    g_rx_irq = true;
    return true;
}

void poll_rx() {
    if (!g_present) return;
    u64 flags = cpu::irq_save();
    g_rx_lock.lock();
    drain_rx();
    g_rx_lock.unlock();
    cpu::irq_restore(flags);
}

static void put_uint(u64 v, u32 base, bool upper, u32 width = 0, bool zero = false) {
    char buf[32];
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;
    if (v == 0) buf[i++] = '0';
    while (v) { buf[i++] = digits[v % base]; v /= base; }
    for (u32 pad = static_cast<u32>(i); pad < width; ++pad) putc_raw(zero ? '0' : ' ');
    while (i--) putc_raw(buf[i]);
}

static void put_int(i64 v, u32 width = 0, bool zero = false) {
    if (v < 0) { putc_raw('-'); put_uint(static_cast<u64>(-v), 10, false, width > 0 ? width - 1 : 0, zero); }
    else put_uint(static_cast<u64>(v), 10, false, width, zero);
}

void printf(const char* fmt, ...) {
    TxGuard guard;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    for (const char* p = fmt; *p; ++p) {
        if (*p != '%') { putc_raw(*p); continue; }
        bool zero = false;
        u32 width = 0;
        if (*(p + 1) == '0') { zero = true; ++p; }
        while (*(p + 1) >= '0' && *(p + 1) <= '9') width = width * 10 + static_cast<u32>(*++p - '0');
        switch (*++p) {
            case 's': write_raw(__builtin_va_arg(ap, const char*)); break;
            case 'c': putc_raw(static_cast<char>(__builtin_va_arg(ap, int))); break;
            case 'd': put_int(__builtin_va_arg(ap, int), width, zero); break;
            case 'u': put_uint(__builtin_va_arg(ap, unsigned), 10, false, width, zero); break;
            case 'x': put_uint(__builtin_va_arg(ap, unsigned), 16, false, width, zero); break;
            case 'p': write_raw("0x"); put_uint(__builtin_va_arg(ap, u64), 16, false); break;
            case 'l':
                if (*(p + 1) == 'u') { ++p; put_uint(__builtin_va_arg(ap, u64), 10, false, width, zero); }
                else if (*(p + 1) == 'x') { ++p; put_uint(__builtin_va_arg(ap, u64), 16, false, width, zero); }
                break;
            case '%': putc_raw('%'); break;
            default: putc_raw('%'); putc_raw(*p); break;
        }
    }
    __builtin_va_end(ap);
}

}
