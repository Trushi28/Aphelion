#include <cosmos/serial.hpp>
#include <cosmos/cpu.hpp>

namespace serial {

static constexpr u16 COM1 = 0x3F8;

void init() {
    cpu::out8(COM1 + 1, 0x00);
    cpu::out8(COM1 + 3, 0x80);
    cpu::out8(COM1 + 0, 0x01);
    cpu::out8(COM1 + 1, 0x00);
    cpu::out8(COM1 + 3, 0x03);
    cpu::out8(COM1 + 2, 0xC7);
    cpu::out8(COM1 + 4, 0x0B);
}

static bool tx_ready() { return cpu::in8(COM1 + 5) & 0x20; }

void putc(char c) {
    if (c == '\n') putc('\r');
    while (!tx_ready()) cpu::io_wait();
    cpu::out8(COM1, static_cast<u8>(c));
}

void write(const char* s) { while (*s) putc(*s++); }
void writeln(const char* s) { write(s); putc('\n'); }

static void put_uint(u64 v, u32 base, bool upper) {
    char buf[32];
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;
    if (v == 0) buf[i++] = '0';
    while (v) { buf[i++] = digits[v % base]; v /= base; }
    while (i--) putc(buf[i]);
}

static void put_int(i64 v) {
    if (v < 0) { putc('-'); put_uint(static_cast<u64>(-v), 10, false); }
    else put_uint(static_cast<u64>(v), 10, false);
}

void printf(const char* fmt, ...) {

    static cpu::Spinlock lock;
    lock.lock();
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    for (const char* p = fmt; *p; ++p) {
        if (*p != '%') { putc(*p); continue; }
        switch (*++p) {
            case 's': write(__builtin_va_arg(ap, const char*)); break;
            case 'c': putc(static_cast<char>(__builtin_va_arg(ap, int))); break;
            case 'd': put_int(__builtin_va_arg(ap, int)); break;
            case 'u': put_uint(__builtin_va_arg(ap, unsigned), 10, false); break;
            case 'x': put_uint(__builtin_va_arg(ap, unsigned), 16, false); break;
            case 'p': write("0x"); put_uint(__builtin_va_arg(ap, u64), 16, false); break;
            case 'l':
                if (*(p + 1) == 'u') { ++p; put_uint(__builtin_va_arg(ap, u64), 10, false); }
                else if (*(p + 1) == 'x') { ++p; put_uint(__builtin_va_arg(ap, u64), 16, false); }
                break;
            case '%': putc('%'); break;
            default: putc('%'); putc(*p); break;
        }
    }
    __builtin_va_end(ap);
    lock.unlock();
}

}
