#include <cosmos/framebuffer.hpp>
#include <cosmos/corona.hpp>
#include <cosmos/kheap.hpp>
#include <cosmos/serial.hpp>

extern "C" const u8 corona_font_blob[];
extern "C" const u8 corona_font_blob_end[];

namespace fb {

namespace {

constexpr usize MAX_BACKBUFFER_BYTES = 96ull << 20;
constexpr u32 SCROLLBACK_LINES = 2000;
constexpr usize MAX_SCROLLBACK_BYTES = 12ull << 20;
constexpr u32 LINE_MAX = 512;

struct Out {
    char buf[LINE_MAX];
    u32 n = 0;
    void put(char c) { if (n + 1 < LINE_MAX) buf[n++] = c; }
    void puts(const char* s) { if (!s) s = "(null)"; while (*s) put(*s++); }
    void number(u64 v, u32 base, bool neg) {
        char tmp[24];
        u32 i = 0;
        const char* digits = "0123456789abcdef";
        if (v == 0) tmp[i++] = '0';
        while (v) { tmp[i++] = digits[v % base]; v /= base; }
        if (neg) put('-');
        while (i) put(tmp[--i]);
    }
};

}

bool init(const Info& info) {
    return corona::init(info, corona_font_blob, static_cast<usize>(corona_font_blob_end - corona_font_blob));
}

bool enable_backbuffer() {
    usize need = corona::backbuffer_bytes_needed();
    if (need == 0 || need > MAX_BACKBUFFER_BYTES || !kheap::ready()) return false;
    void* mem = kheap::alloc(need);
    if (!mem) return false;
    if (!corona::enable_backbuffer(static_cast<u32*>(mem), need)) {
        kheap::free(mem);
        return false;
    }
    return true;
}

u32 enable_scrollback() {
    usize per_line = corona::scrollback_bytes_per_line();
    if (per_line == 0 || !kheap::ready()) return 0;
    usize lines = SCROLLBACK_LINES;
    if (lines * per_line > MAX_SCROLLBACK_BYTES) lines = MAX_SCROLLBACK_BYTES / per_line;
    if (lines == 0) return 0;
    void* mem = kheap::alloc(lines * per_line);
    if (!mem) return 0;
    u32 got = corona::enable_scrollback(static_cast<term::Cell*>(mem), lines * per_line);
    if (got == 0) kheap::free(mem);
    return got;
}

bool ready() { return corona::ready(); }

void enter_panic() { corona::enter_panic(); }

void clear(u32 rgb) { corona::clear(rgb); }

void put_pixel(u64 x, u64 y, u32 rgb) { corona::put_pixel(x, y, rgb); }

void putc(char c, u32 fg, u32 bg) {
    char s[2] = { c, 0 };
    corona::write_colored(s, fg, bg);
}

void write(const char* s, u32 fg, u32 bg) { corona::write_colored(s, fg, bg); }

void writeln(const char* s, u32 fg, u32 bg) {
    corona::write_colored(s, fg, bg);
    corona::write_colored("\n", fg, bg);
}

void printf(u32 fg, const char* fmt, ...) {
    if (!corona::ready()) return;
    Out o;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    for (const char* p = fmt; *p; ++p) {
        if (*p != '%') { o.put(*p); continue; }
        switch (*++p) {
            case 's': o.puts(__builtin_va_arg(ap, const char*)); break;
            case 'c': o.put(static_cast<char>(__builtin_va_arg(ap, int))); break;
            case 'd': {
                int v = __builtin_va_arg(ap, int);
                o.number(v < 0 ? static_cast<u64>(-static_cast<i64>(v)) : static_cast<u64>(v), 10, v < 0);
                break;
            }
            case 'u': o.number(__builtin_va_arg(ap, unsigned), 10, false); break;
            case 'x': o.number(__builtin_va_arg(ap, unsigned), 16, false); break;
            case 'p': o.puts("0x"); o.number(__builtin_va_arg(ap, u64), 16, false); break;
            case 'l':
                if (*(p + 1) == 'u') { ++p; o.number(__builtin_va_arg(ap, u64), 10, false); }
                else if (*(p + 1) == 'x') { ++p; o.number(__builtin_va_arg(ap, u64), 16, false); }
                break;
            case '%': o.put('%'); break;
            case 0: --p; break;
            default: o.put('%'); o.put(*p); break;
        }
    }
    __builtin_va_end(ap);
    o.buf[o.n] = 0;
    corona::write_colored(o.buf, fg, corona::VOID);
}

}
