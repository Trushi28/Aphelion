#include <cosmos/framebuffer.hpp>
#include <cosmos/font8x8.hpp>

namespace fb {

struct Cell { u8 ch; u8 fg; u8 bg; };

static constexpr u64 GLYPH_W = 8, GLYPH_H = 8;
static constexpr u64 MAX_COLS = 512, MAX_ROWS = 300;
static constexpr u32 PALETTE_MAX = 64;
static constexpr u32 DEFAULT_FG = 0xE0E0E0, DEFAULT_BG = 0x0A0A14;

static Info g_info{};
static u64 g_col = 0, g_row = 0;
static u64 g_cols = 0, g_rows = 0;
static bool g_ready = false;
static Cell g_grid[MAX_ROWS * MAX_COLS];
static u32 g_palette[PALETTE_MAX];
static u32 g_palette_n = 0;

bool ready() { return g_ready; }

static inline void flush_writes() { asm volatile("sfence" ::: "memory"); }

static u8 color_index(u32 rgb) {
    for (u32 i = 0; i < g_palette_n; ++i) if (g_palette[i] == rgb) return static_cast<u8>(i);
    if (g_palette_n < PALETTE_MAX) { g_palette[g_palette_n] = rgb; return static_cast<u8>(g_palette_n++); }
    return static_cast<u8>(PALETTE_MAX - 1);
}

static u32 pack(u32 rgb) {
    u32 r = (rgb >> 16) & 0xFF;
    u32 g = (rgb >> 8) & 0xFF;
    u32 b = rgb & 0xFF;
    return (r << g_info.red_shift) | (g << g_info.green_shift) | (b << g_info.blue_shift);
}

static volatile u32* row_ptr(u64 y, u64 x) {
    return reinterpret_cast<volatile u32*>(g_info.base + y * g_info.pitch + x * (g_info.bpp / 8));
}

static Cell& cell(u64 col, u64 row) { return g_grid[row * g_cols + col]; }

static void draw_cell(u64 col, u64 row) {
    const Cell& c = cell(col, row);
    const u8* rows = font8x8::glyphs[c.ch & 0x7F];
    u64 ox = col * GLYPH_W, oy = row * GLYPH_H;
    u32 pf = pack(g_palette[c.fg]), pb = pack(g_palette[c.bg]);
    for (u64 gy = 0; gy < GLYPH_H; ++gy) {
        volatile u32* line = row_ptr(oy + gy, ox);
        u8 bits = rows[gy];
        for (u64 gx = 0; gx < GLYPH_W; ++gx) line[gx] = ((bits >> gx) & 1) ? pf : pb;
    }
}

static void redraw_all() {
    for (u64 r = 0; r < g_rows; ++r)
        for (u64 c = 0; c < g_cols; ++c) draw_cell(c, r);
    flush_writes();
}

void init(const Info& info) {
    g_info = info;
    g_cols = g_info.width / GLYPH_W;
    g_rows = g_info.height / GLYPH_H;
    if (g_cols > MAX_COLS) g_cols = MAX_COLS;
    if (g_rows > MAX_ROWS) g_rows = MAX_ROWS;
    g_col = g_row = 0;
    g_palette_n = 0;
    u8 fg = color_index(DEFAULT_FG), bg = color_index(DEFAULT_BG);
    for (u64 i = 0; i < g_cols * g_rows; ++i) g_grid[i] = { ' ', fg, bg };
    g_ready = true;
}

void put_pixel(u64 x, u64 y, u32 rgb) {
    if (x >= g_info.width || y >= g_info.height) return;
    *row_ptr(y, x) = pack(rgb);
}

static void fill_rows(u64 first, u64 count, u32 packed) {
    for (u64 y = first; y < first + count && y < g_info.height; ++y) {
        void* d = const_cast<u32*>(row_ptr(y, 0));
        u64 n = g_info.width;
        asm volatile("rep stosl" : "+D"(d), "+c"(n) : "a"(packed) : "memory");
    }
}

void clear(u32 rgb) {
    u8 fg = color_index(DEFAULT_FG), bg = color_index(rgb);
    for (u64 i = 0; i < g_cols * g_rows; ++i) g_grid[i] = { ' ', fg, bg };
    fill_rows(0, g_info.height, pack(rgb));
    flush_writes();
    g_col = g_row = 0;
}

static void scroll(u32 bg_rgb) {
    u64 keep = (g_rows - 1) * g_cols;
    memmove(&g_grid[0], &g_grid[g_cols], keep * sizeof(Cell));
    u8 fg = color_index(DEFAULT_FG), bg = color_index(bg_rgb);
    for (u64 c = 0; c < g_cols; ++c) g_grid[keep + c] = { ' ', fg, bg };
    redraw_all();
}

static void put_cell(char ch, u64 col, u64 row, u32 fg, u32 bg) {
    cell(col, row) = { static_cast<u8>(ch), color_index(fg), color_index(bg) };
    draw_cell(col, row);
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
        put_cell(' ', g_col, g_row, fg, bg);
    } else {
        put_cell(c, g_col, g_row, fg, bg);
        if (++g_col >= g_cols) { g_col = 0; ++g_row; }
    }
    if (g_row >= g_rows) {
        scroll(bg);
        g_row = g_rows - 1;
    } else {
        flush_writes();
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
