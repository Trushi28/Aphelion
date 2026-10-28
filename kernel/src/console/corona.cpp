#include <cosmos/corona.hpp>
#include <cosmos/corona_font.hpp>

#ifdef CORONA_HOST_TEST
namespace {
struct Lock {
    void lock() {}
    void unlock() {}
};
}
#else
#include <cosmos/orbital.hpp>
namespace {
using Lock = orbital::Mutex;
}
#endif

namespace corona {

namespace {

constexpr u32 PAD_CELLS_X = 1;

Lock g_lock;
fb::Info g_info;
const font::Face* g_face = nullptr;
term::Terminal g_term;
term::Cell g_cells[term::MAX_COLS * term::MAX_ROWS];
u32* g_back = nullptr;
usize g_back_bytes = 0;
u64 g_pitch_px = 0;
u32 g_cols = 0;
u32 g_rows = 0;
u32 g_origin_x = 0;
u32 g_origin_y = 0;
u32 g_cursor_color = 0;
bool g_ready = false;

bool g_lut_valid = false;
u32 g_lut_fg = 0;
u32 g_lut_bg = 0;
u32 g_lut[16];

bool g_panic = false;
bool g_locking = false;

struct Guard {
    bool held;
    Guard() : held(__atomic_load_n(&g_locking, __ATOMIC_ACQUIRE) && !__atomic_load_n(&g_panic, __ATOMIC_ACQUIRE)) { if (held) g_lock.lock(); }
    ~Guard() { if (held) g_lock.unlock(); }
};

const term::Palette THEME = {
    {
        0x14161F, 0xFF6B7A, 0x9BE38A, 0xFFC96B, 0x6EA8FF, 0xC792EA, 0x5CE1E6, 0xC5CCD6,
        0x4B5263, 0xFF8E9A, 0xB8F2A5, 0xFFE08F, 0x93C0FF, 0xDDB3FF, 0x8CF0F5, 0xF4F7FB,
    },
    TEXT,
    VOID,
};

u32 pack(u32 rgb) {
    u32 r = (rgb >> 16) & 0xFF;
    u32 g = (rgb >> 8) & 0xFF;
    u32 b = rgb & 0xFF;
    return (r << g_info.red_shift) | (g << g_info.green_shift) | (b << g_info.blue_shift);
}

u32 mix(u32 a, u32 b, u32 t) {
    auto ch = [&](u32 shift) -> u32 {
        u32 x = (a >> shift) & 0xFF;
        u32 y = (b >> shift) & 0xFF;
        return (x * (255 - t) + y * t + 127) / 255;
    };
    return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

void build_lut(u32 fg, u32 bg) {
    if (g_lut_valid && g_lut_fg == fg && g_lut_bg == bg) return;
    for (u32 i = 0; i < 16; ++i) g_lut[i] = pack(mix(bg, fg, i * 17));
    g_lut_fg = fg;
    g_lut_bg = bg;
    g_lut_valid = true;
}

void render_cell(u32* dst, u64 pitch, const term::Cell& c, bool cursor) {
    const font::Face& f = *g_face;
    u32 attr = term::cell_attr(c);
    u32 cp = term::cell_cp(c);
    u32 fg = c.fg;
    u32 bg = c.bg;
    if (attr & term::REVERSE) { u32 t = fg; fg = bg; bg = t; }
    if (attr & term::DIM) fg = mix(fg, bg, 100);
    if (cursor) { fg = VOID; bg = g_cursor_color; }
    build_lut(fg, bg);

    u32 w = f.cell_w, h = f.cell_h;
    bool blank = (attr & term::WIDE_TAIL) || cp == 0 || cp == ' ';
    if (blank) {
        u32 p = g_lut[0];
        for (u32 y = 0; y < h; ++y) {
            u32* line = dst + y * pitch;
            for (u32 x = 0; x < w; ++x) line[x] = p;
        }
    } else {
        const u8* g = font::glyph(f, cp, (attr & term::BOLD) != 0, nullptr);
        if ((w & 1) == 0) {

            const u32* lut = g_lut;
            for (u32 y = 0; y < h; ++y) {
                u32* line = dst + y * pitch;
                for (u32 x = 0; x < w; x += 2) {
                    u8 b = *g++;
                    line[x] = lut[b & 15];
                    line[x + 1] = lut[b >> 4];
                }
            }
        } else {
            u32 i = 0;
            for (u32 y = 0; y < h; ++y) {
                u32* line = dst + y * pitch;
                for (u32 x = 0; x < w; ++x, ++i) line[x] = g_lut[font::coverage(g, i)];
            }
        }
    }
    if ((attr & term::UNDERLINE) && !(attr & term::WIDE_TAIL)) {
        u32 y = static_cast<u32>(f.baseline) + 2 < h ? static_cast<u32>(f.baseline) + 2 : h - 1;
        u32 p = g_lut[15];
        u32* line = dst + y * pitch;
        for (u32 x = 0; x < w; ++x) line[x] = p;
    }
}

void fill_px(u32* dst, u32 value, u64 count) {
#ifdef CORONA_HOST_TEST
    for (u64 i = 0; i < count; ++i) dst[i] = value;
#else
    u64 pairs = count >> 1;
    u64 pattern = (static_cast<u64>(value) << 32) | value;
    asm volatile("rep stosq" : "+D"(dst), "+c"(pairs) : "a"(pattern) : "memory");
    if (count & 1) *dst = value;
#endif
}

u32* fb_row(u64 y) {
    return reinterpret_cast<u32*>(const_cast<u8*>(g_info.base) + y * g_info.pitch);
}

void copy_lines_to_fb(u64 y0, u64 lines) {
    if (!g_back) return;
    for (u64 y = y0; y < y0 + lines && y < g_info.height; ++y)
        mem::copy(fb_row(y), g_back + y * g_pitch_px, g_info.width * 4);
}

void copy_span_to_fb(u64 y0, u64 lines, u64 x0, u64 count) {
    if (!g_back) return;
    for (u64 y = y0; y < y0 + lines && y < g_info.height; ++y)
        mem::copy(fb_row(y) + x0, g_back + y * g_pitch_px + x0, count * 4);
}

void fill_surface(u32 rgb) {
    u32 p = pack(rgb);
    for (u64 y = 0; y < g_info.height; ++y) {
        u32* row = fb_row(y);
        for (u64 x = 0; x < g_info.width; ++x) row[x] = p;
        if (g_back) {
            u32* b = g_back + y * g_pitch_px;
            for (u64 x = 0; x < g_info.width; ++x) b[x] = p;
        }
    }
}

bool plain_blank(const term::Cell& c) {
    u32 attr = term::cell_attr(c);
    if (attr & (term::REVERSE | term::UNDERLINE)) return false;
    return (attr & term::WIDE_TAIL) || term::cell_cp(c) == 0 || term::cell_cp(c) == ' ';
}

void draw_row(u32 row, bool cursor_visible, u32 cursor_col, u32 cursor_row) {
    const font::Face& f = *g_face;
    u64 y = g_origin_y + static_cast<u64>(row) * f.cell_h;
    u32 first = g_term.dirty_begin(row);
    u32 last = g_term.dirty_end(row);
    if (last > g_cols) last = g_cols;
    if (first >= last) return;
    auto is_cursor = [&](u32 col) { return cursor_visible && row == cursor_row && col == cursor_col; };
    u32 col = first;
    while (col < last) {
        const term::Cell& c = g_term.view_cell(col, row);
        if (!is_cursor(col) && plain_blank(c)) {
            u32 end = col + 1;
            while (end < last && !is_cursor(end)) {
                const term::Cell& n = g_term.view_cell(end, row);
                if (!plain_blank(n) || n.bg != c.bg) break;
                ++end;
            }
            u32 pixel = pack(c.bg);
            u64 x = g_origin_x + static_cast<u64>(col) * f.cell_w;
            u64 width = static_cast<u64>(end - col) * f.cell_w;
            for (u32 line = 0; line < f.cell_h; ++line) {
                u32* dst = g_back ? g_back + (y + line) * g_pitch_px + x : fb_row(y + line) + x;
                fill_px(dst, pixel, width);
            }
            col = end;
            continue;
        }
        u64 x = g_origin_x + static_cast<u64>(col) * f.cell_w;
        if (g_back) render_cell(g_back + y * g_pitch_px + x, g_pitch_px, c, is_cursor(col));
        else render_cell(fb_row(y) + x, g_info.pitch / 4, c, is_cursor(col));
        ++col;
    }
    if (g_back) copy_span_to_fb(y, f.cell_h, g_origin_x + static_cast<u64>(first) * f.cell_w, static_cast<u64>(last - first) * f.cell_w);
}

void flush_locked() {
    if (!g_ready) return;
    const font::Face& f = *g_face;
    i32 scrolled = g_term.take_scroll();
    if (scrolled > 0) {
        if (g_back) {
            u64 grid_h = static_cast<u64>(g_rows) * f.cell_h;
            u64 shift = static_cast<u64>(scrolled) * f.cell_h;
            if (shift < grid_h) {
                mem::move(g_back + static_cast<u64>(g_origin_y) * g_pitch_px,
                        g_back + (static_cast<u64>(g_origin_y) + shift) * g_pitch_px,
                        (grid_h - shift) * g_pitch_px * 4);
            }
        } else {
            g_term.mark_all_dirty();
        }
    }
    bool cur_vis = g_term.cursor_visible() && g_term.view_offset() == 0;
    u32 cc = g_term.cursor_col();
    u32 cr = g_term.cursor_row();
    for (u32 r = 0; r < g_rows; ++r) {
        if (g_term.row_dirty(r)) draw_row(r, cur_vis, cc, cr);
    }
    if (scrolled > 0 && g_back) copy_lines_to_fb(g_origin_y, static_cast<u64>(g_rows) * f.cell_h);
    g_term.clear_dirty();
    asm volatile("" ::: "memory");
#ifndef CORONA_HOST_TEST
    asm volatile("sfence" ::: "memory");
#endif
}

}

const term::Palette& theme() { return THEME; }

bool init(const fb::Info& info, const u8* font_blob, usize font_length) {
    Guard g;
    g_ready = false;
    if (info.bpp != 32 || info.width < 64 || info.height < 64 || info.pitch < info.width * 4 || (info.pitch & 3)) return false;
    if (!font::load(font_blob, font_length)) return false;
    g_info = info;
    g_face = &font::face(font::pick(info.width, info.height));
    g_back = nullptr;
    g_back_bytes = 0;
    g_pitch_px = info.width;
    g_cursor_color = AMBER;
    g_lut_valid = false;

    u32 cw = g_face->cell_w, ch = g_face->cell_h;
    u64 usable_w = info.width > 2ull * cw * PAD_CELLS_X ? info.width - 2ull * cw * PAD_CELLS_X : info.width;
    u64 usable_h = info.height > ch ? info.height - ch : info.height;
    g_cols = static_cast<u32>(usable_w / cw);
    g_rows = static_cast<u32>(usable_h / ch);
    if (g_cols > term::MAX_COLS) g_cols = term::MAX_COLS;
    if (g_rows > term::MAX_ROWS) g_rows = term::MAX_ROWS;
    if (g_cols == 0 || g_rows == 0) return false;
    g_origin_x = static_cast<u32>((info.width - static_cast<u64>(g_cols) * cw) / 2);
    g_origin_y = static_cast<u32>((info.height - static_cast<u64>(g_rows) * ch) / 2);

    g_term.init(g_cells, g_cols, g_rows, THEME);
    fill_surface(VOID);
    g_ready = true;
    flush_locked();
    return true;
}

bool ready() { return g_ready; }

usize scrollback_bytes_per_line() { return g_ready ? static_cast<usize>(g_cols) * sizeof(term::Cell) : 0; }

u32 enable_scrollback(term::Cell* memory, usize bytes) {
    Guard g;
    if (!g_ready || !memory) return 0;
    usize per_line = static_cast<usize>(g_cols) * sizeof(term::Cell);
    usize lines = bytes / per_line;
    if (lines > 0xFFFFFF) lines = 0xFFFFFF;
    if (lines == 0) return 0;
    g_term.set_history(memory, static_cast<u32>(lines));
    return static_cast<u32>(lines);
}

bool scroll_view(i32 lines) {
    if (!g_ready) return false;
    Guard g;
    bool moved = g_term.scroll_view(lines);
    if (moved) flush_locked();
    return moved;
}

bool snap_to_live() {
    if (!g_ready) return false;
    Guard g;
    bool moved = g_term.snap_to_live();
    if (moved) flush_locked();
    return moved;
}

u32 scrollback_lines() { return g_ready ? g_term.history_count() : 0; }

u32 view_offset() { return g_ready ? g_term.view_offset() : 0; }

usize backbuffer_bytes_needed() { return g_ready ? static_cast<usize>(g_info.width) * g_info.height * 4 : 0; }

bool enable_backbuffer(u32* memory, usize bytes) {
    Guard g;
    if (!g_ready || !memory || bytes < static_cast<usize>(g_info.width) * g_info.height * 4) return false;
    g_back = memory;
    g_back_bytes = bytes;
    g_pitch_px = g_info.width;
    for (u64 y = 0; y < g_info.height; ++y) {
        u32 p = pack(VOID);
        u32* b = g_back + y * g_pitch_px;
        for (u64 x = 0; x < g_info.width; ++x) b[x] = p;
    }
    g_term.mark_all_dirty();
    flush_locked();
    copy_lines_to_fb(0, g_info.height);
    return true;
}

Metrics metrics() {
    Metrics m{};
    if (!g_ready) return m;
    m.cols = g_cols;
    m.rows = g_rows;
    m.cell_w = g_face->cell_w;
    m.cell_h = g_face->cell_h;
    m.fb_width = g_info.width;
    m.fb_height = g_info.height;
    m.backbuffer = g_back != nullptr;
    return m;
}

term::Terminal& terminal() { return g_term; }

void write(const char* s, u32 n) {
    if (!g_ready) return;
    Guard g;
    g_term.write(s, n);
    flush_locked();
}

void write(const char* s) {
    if (!g_ready) return;
    u32 n = 0;
    while (s[n]) ++n;
    write(s, n);
}

void write_colored(const char* s, u32 fg, u32 bg) {
    if (!g_ready) return;
    Guard g;
    g_term.set_colors(fg, bg);
    u32 n = 0;
    while (s[n]) ++n;
    g_term.write(s, n);
    g_term.reset_colors();
    flush_locked();
}

void put_pixel(u64 x, u64 y, u32 rgb) {
    if (!g_ready || x >= g_info.width || y >= g_info.height) return;
    Guard g;
    u32 p = pack(rgb);
    if (g_back) g_back[y * g_pitch_px + x] = p;
    fb_row(y)[x] = p;
}

void clear(u32 bg_rgb) {
    if (!g_ready) return;
    Guard g;
    term::Palette pal = THEME;
    pal.default_bg = bg_rgb;
    g_term.init(g_cells, g_cols, g_rows, pal);
    fill_surface(bg_rgb);
    g_term.mark_all_dirty();
    flush_locked();
}

void flush() {
    Guard g;
    flush_locked();
}

void set_cursor_color(u32 rgb) {
    Guard g;
    g_cursor_color = rgb;
    g_term.mark_all_dirty();
}

u32 pixel_at(u64 x, u64 y) {
    if (!g_ready || x >= g_info.width || y >= g_info.height) return 0;
    if (g_back) return g_back[y * g_pitch_px + x];
    return fb_row(y)[x];
}

bool selftest() {
    if (!g_ready) return false;
    Guard g;
    const font::Face& f = *g_face;
    term::Terminal& t = g_term;
    u32 col0 = t.cursor_col(), row0 = t.cursor_row();
    if (col0 != 0) return false;

    t.write("\x1b[s\r\x1b[2KA\xE2\x82\xB9\xCE\xA9\xD0\x96\xE2\x95\x94\xE4\xB8\xAD\xF0\x9F\x98\x80z");
    flush_locked();

    bool ok = true;
    u32 r = t.cursor_row();
    ok = ok && term::cell_cp(t.cell(0, r)) == 'A';
    ok = ok && term::cell_cp(t.cell(1, r)) == 0x20B9;
    ok = ok && term::cell_cp(t.cell(2, r)) == 0x3A9;
    ok = ok && term::cell_cp(t.cell(3, r)) == 0x416;
    ok = ok && term::cell_cp(t.cell(4, r)) == 0x2554;
    ok = ok && (term::cell_attr(t.cell(5, r)) & term::WIDE_LEAD) && (term::cell_attr(t.cell(6, r)) & term::WIDE_TAIL);
    ok = ok && term::cell_cp(t.cell(7, r)) == 0x1F600 && term::cell_cp(t.cell(9, r)) == 'z';
    ok = ok && font::has(f, 0x20B9) && font::has(f, 0x2554) && font::has(f, 0x2800) && font::has(f, 0x416);

    u64 x0 = g_origin_x;
    u64 y0 = g_origin_y + static_cast<u64>(r) * f.cell_h;
    u64 ink = 0;
    u32 bgp = pack(VOID);
    for (u32 y = 0; y < f.cell_h; ++y)
        for (u32 x = 0; x < f.cell_w; ++x)
            if (pixel_at(x0 + x, y0 + y) != bgp) ++ink;
    ok = ok && ink > 8 && ink < static_cast<u64>(f.cell_w) * f.cell_h;

    u64 tail_ink = 0;
    u64 x6 = g_origin_x + 6ull * f.cell_w;
    for (u32 y = 0; y < f.cell_h; ++y)
        for (u32 x = 0; x < f.cell_w; ++x)
            if (pixel_at(x6 + x, y0 + y) != bgp) ++tail_ink;
    ok = ok && tail_ink == 0;

    t.write("\r\x1b[2K\x1b[u");
    flush_locked();
    ok = ok && t.cursor_col() == col0 && t.cursor_row() == row0;
    return ok;
}

void enable_locking() { __atomic_store_n(&g_locking, true, __ATOMIC_RELEASE); }

void enter_panic() { __atomic_store_n(&g_panic, true, __ATOMIC_RELEASE); }

}
