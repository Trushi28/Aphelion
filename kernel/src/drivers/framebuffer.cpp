#include <cosmos/framebuffer.hpp>
#include <cosmos/font8x8.hpp>

namespace fb {

static Info g_info{};
static u64 g_col = 0, g_row = 0;
static u64 g_cols = 0, g_rows = 0;
static bool g_ready = false;

static constexpr u64 GLYPH_W = 8, GLYPH_H = 8;

bool ready() { return g_ready; }

void init(const Info& info) {
    g_info = info;
    g_cols = g_info.width / GLYPH_W;
    g_rows = g_info.height / GLYPH_H;
    g_col = g_row = 0;
    g_ready = true;
}

void put_pixel(u64 x, u64 y, u32 rgb) {
    if (x >= g_info.width || y >= g_info.height) return;
    u8 r = static_cast<u8>((rgb >> 16) & 0xFF);
    u8 g = static_cast<u8>((rgb >> 8) & 0xFF);
    u8 b = static_cast<u8>(rgb & 0xFF);
    u32 packed = (static_cast<u32>(r) << g_info.red_shift) |
                 (static_cast<u32>(g) << g_info.green_shift) |
                 (static_cast<u32>(b) << g_info.blue_shift);
    auto* px = reinterpret_cast<volatile u32*>(g_info.base + y * g_info.pitch + x * (g_info.bpp / 8));
    *px = packed;
}

void clear(u32 rgb) {
    for (u64 y = 0; y < g_info.height; ++y)
        for (u64 x = 0; x < g_info.width; ++x)
            put_pixel(x, y, rgb);
    g_col = g_row = 0;
}

static void scroll(u32 bg) {

    u64 row_bytes = GLYPH_H * g_info.pitch;
    for (u64 y = 0; y < g_info.height - GLYPH_H; ++y) {
        auto* dst = g_info.base + y * g_info.pitch;
        auto* src = g_info.base + (y + GLYPH_H) * g_info.pitch;
        for (u64 i = 0; i < g_info.pitch; ++i) dst[i] = src[i];
    }
    (void)row_bytes;
    for (u64 y = g_info.height - GLYPH_H; y < g_info.height; ++y)
        for (u64 x = 0; x < g_info.width; ++x)
            put_pixel(x, y, bg);
}

static void draw_glyph(char c, u64 col, u64 row, u32 fg, u32 bg) {
    const u8* rows = font8x8::glyphs[static_cast<u8>(c) & 0x7F];
    u64 ox = col * GLYPH_W, oy = row * GLYPH_H;
    for (u64 gy = 0; gy < GLYPH_H; ++gy) {
        u8 bits = rows[gy];
        for (u64 gx = 0; gx < GLYPH_W; ++gx) {
            bool on = (bits >> gx) & 1;
            put_pixel(ox + gx, oy + gy, on ? fg : bg);
        }
    }
}

void putc(char c, u32 fg, u32 bg) {
    if (!g_ready) return;
    if (c == '\n') {
        g_col = 0; ++g_row;
    } else if (c == '\r') {
        g_col = 0;
    } else if (c == '\b') {
        if (g_col > 0) --g_col;
        else if (g_row > 0) { --g_row; g_col = g_cols - 1; }
        draw_glyph(' ', g_col, g_row, fg, bg);
    } else {
        draw_glyph(c, g_col, g_row, fg, bg);
        if (++g_col >= g_cols) { g_col = 0; ++g_row; }
    }
    if (g_row >= g_rows) {
        scroll(bg);
        g_row = g_rows - 1;
    }
}

void write(const char* s, u32 fg, u32 bg) { while (*s) putc(*s++, fg, bg); }
void writeln(const char* s, u32 fg, u32 bg) { write(s, fg, bg); putc('\n', fg, bg); }

static void put_uint(u32 fg, u32 bg, u64 v, u32 base) {
    char buf[32]; int i = 0;
    const char* digits = "0123456789abcdef";
    if (v == 0) buf[i++] = '0';
    while (v) { buf[i++] = digits[v % base]; v /= base; }
    while (i--) putc(buf[i], fg, bg);
}

void printf(u32 fg, const char* fmt, ...) {
    u32 bg = 0x0A0A14;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    for (const char* p = fmt; *p; ++p) {
        if (*p != '%') { putc(*p, fg, bg); continue; }
        switch (*++p) {
            case 's': write(__builtin_va_arg(ap, const char*), fg, bg); break;
            case 'd': put_uint(fg, bg, static_cast<u64>(__builtin_va_arg(ap, int)), 10); break;
            case 'u': put_uint(fg, bg, __builtin_va_arg(ap, unsigned), 10); break;
            case 'x': put_uint(fg, bg, __builtin_va_arg(ap, unsigned), 16); break;
            case 'p': write("0x", fg, bg); put_uint(fg, bg, __builtin_va_arg(ap, u64), 16); break;
            case 'l':
                if (*(p + 1) == 'u') { ++p; put_uint(fg, bg, __builtin_va_arg(ap, u64), 10); }
                else if (*(p + 1) == 'x') { ++p; put_uint(fg, bg, __builtin_va_arg(ap, u64), 16); }
                break;
            case '%': putc('%', fg, bg); break;
            default: putc('%', fg, bg); putc(*p, fg, bg); break;
        }
    }
    __builtin_va_end(ap);
}

}
