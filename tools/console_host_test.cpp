#define CORONA_HOST_TEST 1
#include <cosmos/types.hpp>
#include "../kernel/src/console/corona_font.cpp"
#include "../kernel/src/console/term.cpp"
#include "../kernel/src/console/corona.cpp"
#include <stdio.h>
#include <stdlib.h>
#include <initializer_list>

template <typename T>
struct Vec {
    T* p = nullptr;
    size_t n = 0;
    size_t cap = 0;
    Vec() {}
    Vec(const Vec& o) { *this = o; }
    Vec& operator=(const Vec& o) {
        if (this == &o) return *this;
        free(p);
        p = static_cast<T*>(malloc((o.n ? o.n : 1) * sizeof(T)));
        n = cap = o.n;
        if (o.n) mem::copy(p, o.p, o.n * sizeof(T));
        return *this;
    }
    ~Vec() { free(p); }
    void push_back(T v) {
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            p = static_cast<T*>(realloc(p, cap * sizeof(T)));
        }
        p[n++] = v;
    }
    void assign(size_t count, T v) {
        free(p);
        p = static_cast<T*>(malloc((count ? count : 1) * sizeof(T)));
        n = cap = count;
        for (size_t i = 0; i < count; ++i) p[i] = v;
    }
    void resize(size_t count) {
        if (count > cap) { cap = count; p = static_cast<T*>(realloc(p, cap * sizeof(T))); }
        n = count;
    }
    size_t size() const { return n; }
    T* data() { return p; }
    const T* data() const { return p; }
    T& operator[](size_t i) { return p[i]; }
    const T& operator[](size_t i) const { return p[i]; }
    bool operator==(const Vec& o) const { return n == o.n && (n == 0 || mem::compare(p, o.p, n * sizeof(T)) == 0); }
    bool operator!=(const Vec& o) const { return !(*this == o); }
};

struct Str {
    char b[2048];
    Str() { b[0] = 0; }
    Str(const char* s) { snprintf(b, sizeof(b), "%s", s); }
    bool empty() const { return b[0] == 0; }
    bool operator==(const char* s) const { return cstr::compare(b, s) == 0; }
    const char* c_str() const { return b; }
    void append(const char* s) {
        size_t l = cstr::length(b);
        snprintf(b + l, sizeof(b) - l, "%s", s);
    }
    Str& operator+=(const char* s) { append(s); return *this; }
    size_t size() const { return cstr::length(b); }
};

struct Bytes {
    u8 b[8];
    u32 n = 0;
};

struct Words {
    u32 v[16];
    u32 n = 0;
    Words() {}
    Words(const u32* a, u32 count) { n = count; for (u32 i = 0; i < count; ++i) v[i] = a[i]; }
    bool is(std::initializer_list<u32> l) const {
        if (l.size() != n) return false;
        u32 i = 0;
        for (u32 x : l) if (v[i++] != x) return false;
        return true;
    }
};

static int g_failures = 0;
static Vec<u8> g_blob;

static void expect(bool cond, const char* what) {
    printf("%-72s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) ++g_failures;
}

static bool load_blob() {
    FILE* f = fopen("kernel/data/corona-fonts.bin", "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_blob.resize(static_cast<size_t>(n));
    size_t got = fread(g_blob.data(), 1, static_cast<size_t>(n), f);
    fclose(f);
    return got == static_cast<size_t>(n);
}

static Words decode(std::initializer_list<u8> bytes) {
    utf8::Decoder d;
    u32 tmp[2];
    u32 out[16];
    u32 n = 0;
    for (u8 b : bytes) {
        u32 k = d.feed(b, tmp);
        for (u32 i = 0; i < k && n < 16; ++i) out[n++] = tmp[i];
    }
    u32 tail[1];
    if (d.finish(tail) && n < 16) out[n++] = tail[0];
    return Words(out, n);
}

static void utf8_tests() {
    bool roundtrip = true;
    const u32 samples[] = {0x24, 0x7F, 0x80, 0xA2, 0x7FF, 0x800, 0x20B9, 0xFFFD, 0xFFFF, 0x10000, 0x1F600, 0x10FFFF};
    for (u32 cp : samples) {
        char buf[4];
        u32 n = utf8::encode(cp, buf);
        utf8::Decoder d;
        u32 tmp[2];
        u32 got = 0, count = 0;
        for (u32 i = 0; i < n; ++i) {
            u32 k = d.feed(static_cast<u8>(buf[i]), tmp);
            if (k) { got = tmp[0]; ++count; }
        }
        if (count != 1 || got != cp) roundtrip = false;
    }
    expect(roundtrip, "utf8: encode and decode round-trip across every length");
    expect(decode({0xC0, 0x80}).is({0xFFFD, 0xFFFD}), "utf8: overlong two-byte form is rejected");
    expect(decode({0xE0, 0x80, 0x80}).is({0xFFFD}), "utf8: overlong three-byte form is rejected");
    expect(decode({0xED, 0xA0, 0x80}).is({0xFFFD}), "utf8: UTF-16 surrogates are rejected");
    expect(decode({0xF4, 0x90, 0x80, 0x80}).is({0xFFFD}), "utf8: code points above U+10FFFF are rejected");
    expect(decode({0xF8, 0x88, 0x80, 0x80, 0x80}).n == 5, "utf8: five and six byte forms produce one replacement per byte");
    expect(decode({0xE2, 0x82, 'A'}).is({0xFFFD, 'A'}), "utf8: a truncated sequence does not swallow the next character");
    expect(decode({0xE2, 0x82}).is({0xFFFD}), "utf8: a sequence cut by the end of input is one replacement");
    expect(decode({0x80, 'x'}).is({0xFFFD, 'x'}), "utf8: a stray continuation byte is one replacement");
    expect(decode({0xE2, 0x82, 0xB9}).is({0x20B9}), "utf8: the rupee sign decodes");
    expect(utf8::width('a') == 1 && utf8::width(0x4E2D) == 2 && utf8::width(0x301) == 0 && utf8::width(0x1F600) == 2 && utf8::width(0x2554) == 1,
           "utf8: display widths for narrow, wide and combining characters");
    expect(utf8::cells("a\xE4\xB8\xAD\xCC\x81", 6) == 2, "utf8: combining marks add no cells");
}

static term::Cell g_grid[40 * 12];
static term::Terminal g_t;

static term::Palette test_palette() { return corona::theme(); }

static void fresh(u32 cols = 20, u32 rows = 6) {
    g_t.init(g_grid, cols, rows, test_palette());
}

static Str row(u32 r) {
    Str s;
    g_t.row_text(r, s.b, sizeof(s.b));
    return s;
}

static bool glyph_is(u32 col, u32 r, u32 cp, u32 attr = 0) {
    return g_t.cell(col, r).glyph == term::make_glyph(cp, attr);
}

static void term_tests() {
    fresh();
    g_t.write("hello");
    expect(row(0) == "hello" && g_t.cursor_col() == 5 && g_t.cursor_row() == 0, "term: plain text and cursor position");
    g_t.write("\nab");
    expect(row(1) == "ab" && g_t.cursor_col() == 2, "term: newline returns to column zero");

    fresh(5, 3);
    g_t.write("12345");
    expect(g_t.cursor_col() == 4 && g_t.cursor_row() == 0, "term: writing the last column defers the wrap");
    g_t.write("6");
    expect(row(0) == "12345" && row(1) == "6" && g_t.cursor_row() == 1, "term: the next character wraps to the next row");
    g_t.write("\n\n\n\n");
    expect(g_t.cursor_row() == 2, "term: the cursor stays on the last row while scrolling");

    fresh(10, 3);
    g_t.write("a\nb\nc\nd");
    expect(row(0) == "b" && row(1) == "c" && row(2) == "d", "term: scrolling drops the oldest line");
    expect(g_t.take_scroll() == 1, "term: scroll distance is recorded for the renderer");

    fresh();
    g_t.write("abc\x1b[2D" "X");
    expect(row(0) == "aXc", "term: cursor left then overwrite");
    g_t.write("\x1b[3;5H" "Q");
    expect(glyph_is(4, 2, 'Q'), "term: absolute cursor position, one based");
    g_t.write("\x1b[H\x1b[2J");
    expect(row(0).empty() && row(2).empty() && g_t.cursor_col() == 0 && g_t.cursor_row() == 0, "term: erase display homes nothing and clears everything");

    fresh();
    g_t.write("abcdef\r\x1b[3C\x1b[K");
    expect(row(0) == "abc", "term: erase to end of line");
    g_t.write("\r\x1b[2K");
    expect(row(0).empty(), "term: erase whole line");
    g_t.write("abcdef\x1b[3D\x1b[1K");
    expect(row(0) == "    ef", "term: erase to start of line includes the cursor cell");

    fresh();
    g_t.write("\x1b[31mR\x1b[0mN");
    expect(glyph_is(0, 0, 'R') && g_t.cell(0, 0).fg == test_palette().ansi[1] && g_t.cell(1, 0).fg == test_palette().default_fg,
           "term: SGR sets and resets the foreground");
    g_t.write("\x1b[1;4;7mB\x1b[22;24;27mx");
    u32 a0 = term::cell_attr(g_t.cell(2, 0)), a1 = term::cell_attr(g_t.cell(3, 0));
    expect((a0 & term::BOLD) && (a0 & term::UNDERLINE) && (a0 & term::REVERSE) && a1 == 0, "term: bold, underline and reverse switch on and off");
    g_t.write("\x1b[38;2;10;20;30mT\x1b[48;5;196mU\x1b[38;5;244mV");
    expect(g_t.cell(4, 0).fg == 0x0A141E, "term: 24-bit foreground");
    expect(g_t.cell(5, 0).bg == 0xFF0000, "term: 256-colour cube background");
    expect(g_t.cell(6, 0).fg == 0x808080, "term: 256-colour grey ramp");
    g_t.write("\x1b[92mG\x1b[104mH");
    expect(g_t.cell(7, 0).fg == test_palette().ansi[10] && g_t.cell(8, 0).bg == test_palette().ansi[12], "term: bright foreground and background");
    g_t.write("\x1b[39;49mZ");
    expect(g_t.cell(9, 0).fg == test_palette().default_fg && g_t.cell(9, 0).bg == test_palette().default_bg, "term: default colour codes");

    fresh();
    g_t.write("a\xE4\xB8\xAD" "b");
    expect(term::cell_cp(g_t.cell(1, 0)) == 0x4E2D && (term::cell_attr(g_t.cell(1, 0)) & term::WIDE_LEAD) && (term::cell_attr(g_t.cell(2, 0)) & term::WIDE_TAIL) &&
           term::cell_cp(g_t.cell(3, 0)) == 'b', "term: a wide character takes two cells");
    expect(row(0) == "a\xE4\xB8\xAD" "b", "term: row text skips the tail of a wide character");
    g_t.write("\r\x1b[2Cx");
    expect(term::cell_cp(g_t.cell(1, 0)) == ' ' && !(term::cell_attr(g_t.cell(1, 0)) & term::WIDE_LEAD), "term: overwriting the tail blanks the lead");
    fresh(6, 3);
    g_t.write("12345\xE4\xB8\xAD");
    expect(g_t.cursor_row() == 1 && term::cell_cp(g_t.cell(0, 1)) == 0x4E2D, "term: a wide character that does not fit wraps whole");
    fresh();
    g_t.write("e\xCC\x81z");
    expect(row(0) == "ez", "term: combining marks are dropped without moving the cursor");
    g_t.write("\xFF");
    expect(term::cell_cp(g_t.cell(2, 0)) == 0xFFFD, "term: an invalid byte prints the replacement character");

    fresh();
    g_t.write("abcdef\r\x1b[2C\x1b[2P");
    expect(row(0) == "abef", "term: delete characters");
    g_t.write("\r\x1b[1@");
    expect(row(0) == " abef", "term: insert characters");
    g_t.write("\r\x1b[3X");
    expect(row(0) == "   ef", "term: erase characters");

    fresh(10, 5);
    g_t.write("1\n2\n3\n4\n5");
    g_t.write("\x1b[2;1H\x1b[L");
    expect(row(1).empty() && row(2) == "2" && row(4) == "4", "term: insert line pushes the rest down");
    g_t.write("\x1b[M");
    expect(row(1) == "2" && row(4).empty(), "term: delete line pulls the rest up");

    fresh(10, 6);
    g_t.write("\x1b[2;4r\x1b[2;1Ha\nb\nc\nd");
    g_t.write("\x1b[H" "top");
    expect(row(0) == "top", "term: text above the scroll region is untouched");
    expect(row(1) == "b" && row(2) == "c" && row(3) == "d", "term: scrolling stays inside the region");
    g_t.write("\x1b[r");
    g_t.write("\x1b[6;1Hend");
    expect(row(5) == "end", "term: region reset restores the whole screen");

    fresh();
    g_t.write("\x1b[3;4H\x1b[31m\x1b" "7" "\x1b[1;1H\x1b[0m\x1b" "8" "X");
    expect(term::cell_cp(g_t.cell(3, 2)) == 'X' && g_t.cell(3, 2).fg == test_palette().ansi[1], "term: save and restore cursor with attributes");

    fresh();
    g_t.write("a\tb");
    expect(g_t.cursor_col() == 9 && term::cell_cp(g_t.cell(8, 0)) == 'b', "term: tab stops every eight columns");

    fresh();
    g_t.write("\x1b]0;a title that must not appear\x07ok");
    expect(row(0) == "ok", "term: operating system commands are swallowed");
    g_t.write("\x1b]2;x\x1b\\!");
    expect(row(0) == "ok!", "term: string terminator ends an operating system command");
    g_t.write("\x1b(B\x1b[?25l");
    expect(!g_t.cursor_visible(), "term: cursor can be hidden");
    g_t.write("\x1b[?25h");
    expect(g_t.cursor_visible(), "term: cursor can be shown again");
    g_t.write("\x1b[999;999H");
    expect(g_t.cursor_col() == g_t.cols() - 1 && g_t.cursor_row() == g_t.rows() - 1, "term: cursor positions are clamped");
    g_t.write("\x1b[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;20mQ");
    expect(term::cell_cp(g_t.cell(g_t.cols() - 1, g_t.rows() - 1)) == 'Q', "term: too many parameters are tolerated");
    g_t.write("\x1b[?7l\x1b[1;1H");
    for (int i = 0; i < 40; ++i) g_t.write("w");
    expect(g_t.cursor_row() == 0 && term::cell_cp(g_t.cell(g_t.cols() - 1, 0)) == 'w', "term: with autowrap off the last column is overwritten");
}

static term::Cell g_hist[20 * 64];

static Str view_row(u32 r) {
    Str s;
    u32 n = 0, last = 0;
    char tmp[64];
    for (u32 c = 0; c < g_t.cols() && n + 1 < sizeof(tmp); ++c) {
        u32 cp = term::cell_cp(g_t.view_cell(c, r));
        tmp[n++] = cp ? static_cast<char>(cp) : ' ';
        if (cp != ' ' && cp != 0) last = n;
    }
    tmp[last] = 0;
    snprintf(s.b, sizeof(s.b), "%s", tmp);
    return s;
}

static void history_tests() {
    fresh(10, 3);
    g_t.set_history(g_hist, 5);
    g_t.write("a\nb\nc\nd\ne\nf");
    expect(g_t.history_count() == 3 && row(0) == "d" && row(2) == "f", "history: lines that scroll off the top are kept");
    expect(!g_t.scroll_view(-1) && g_t.view_offset() == 0, "history: there is nothing below the live screen");
    expect(g_t.scroll_view(1) && g_t.view_offset() == 1, "history: scrolling back moves the view");
    expect(view_row(0) == "c" && view_row(1) == "d" && view_row(2) == "e", "history: one line back shows the last kept line on top");
    expect(g_t.scroll_view(50) && g_t.view_offset() == 3, "history: the view stops at the oldest line");
    expect(view_row(0) == "a" && view_row(1) == "b" && view_row(2) == "c", "history: the oldest lines are intact");
    expect(!g_t.scroll_view(1), "history: scrolling past the oldest line changes nothing");
    expect(g_t.scroll_view(-2) && view_row(0) == "c" && view_row(2) == "e", "history: scrolling forward again");
    g_t.write("!");
    expect(g_t.view_offset() == 0 && row(2) == "f!", "history: new output returns the view to the bottom");
    expect(g_t.row_dirty(0) && g_t.row_dirty(2), "history: returning to the bottom redraws every row");

    fresh(10, 3);
    g_t.set_history(g_hist, 5);
    char line[16];
    for (int i = 0; i < 12; ++i) {
        snprintf(line, sizeof(line), "l%d\n", i);
        g_t.write(line);
    }
    expect(g_t.history_count() == 5, "history: the ring never holds more than its capacity");
    g_t.scroll_view(100);
    expect(g_t.view_offset() == 5 && view_row(0) == "l5" && view_row(1) == "l6" && view_row(2) == "l7", "history: a wrapped ring still reads oldest first");
    g_t.scroll_view(-1);
    expect(view_row(0) == "l6" && view_row(2) == "l8", "history: reading across the wrap point");
    g_t.snap_to_live();
    expect(g_t.view_offset() == 0 && row(0) == "l10", "history: snapping returns to the live rows");

    fresh(10, 6);
    g_t.set_history(g_hist, 8);
    g_t.write("\x1b[2;4r\x1b[2;1Ha\nb\nc\nd\ne");
    expect(g_t.history_count() == 0, "history: scrolling inside a region keeps nothing");
    g_t.write("\x1b[r\x1b[6;1H\nx");
    expect(g_t.history_count() == 1, "history: a full-screen scroll keeps the line that left");

    fresh(10, 3);
    g_t.set_history(g_hist, 5);
    g_t.write("a\nb\nc\nd\ne");
    g_t.init(g_grid, 10, 3, test_palette());
    expect(g_t.history_count() == 0 && g_t.view_offset() == 0, "history: reinitialising the terminal forgets it");

    fresh(10, 3);
    g_t.clear_dirty();
    g_t.write("\x1b[2J");
    expect(!g_t.any_dirty(), "term: clearing a blank screen redraws nothing");
    g_t.write("ab");
    g_t.clear_dirty();
    g_t.write("\x1b[2J");
    expect(g_t.row_dirty(0) && !g_t.row_dirty(1) && !g_t.row_dirty(2), "term: clearing redraws only the rows that held something");
    g_t.write("\x1b[H");
    g_t.clear_dirty();
    g_t.write("\x1b[1;32m\x1b[2K");
    expect(g_t.row_dirty(0), "term: erasing with a different colour still redraws");
    g_t.init(g_grid, 10, 3, test_palette());
}

struct Screen {
    u32 w, h;
    Vec<u32> fbuf;
    Vec<u32> back;
    fb::Info info;
    Screen(u32 width, u32 height, bool backbuffer, u32 pitch_extra = 0) : w(width), h(height) {
        u32 pitch_px = width + pitch_extra;
        fbuf.assign(static_cast<size_t>(pitch_px) * height, 0xDEADBEEF);
        info.base = reinterpret_cast<volatile u8*>(fbuf.data());
        info.width = width;
        info.height = height;
        info.pitch = pitch_px * 4ull;
        info.bpp = 32;
        info.red_shift = 16;
        info.green_shift = 8;
        info.blue_shift = 0;
        if (!corona::init(info, g_blob.data(), g_blob.size())) {
            printf("corona init failed\n");
            exit(2);
        }
        if (backbuffer) {
            back.assign(static_cast<size_t>(width) * height, 0);
            corona::enable_backbuffer(back.data(), back.size() * 4);
        }
    }
    Vec<u32> visible() const {
        Vec<u32> out;
        u32 pitch_px = static_cast<u32>(info.pitch / 4);
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x) out.push_back(fbuf[static_cast<size_t>(y) * pitch_px + x]);
        return out;
    }
    u32 at(u32 x, u32 y) const { return fbuf[static_cast<size_t>(y) * (info.pitch / 4) + x]; }
};

static Vec<u32> full_redraw_snapshot(Screen& s) {
    corona::terminal().mark_all_dirty();
    corona::flush();
    return s.visible();
}

static void random_stream(Str& out, unsigned& seed, int n) {
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    const char* words[] = {"Aphelion", "orbital", "\xE2\x95\x94\xE2\x95\x90\xE2\x95\x97", "\xCE\xB1\xCE\xB2\xCE\xB3", "\xE4\xB8\xAD\xE6\x96\x87", "\xE2\x82\xB9" "42",
                           "\xE2\xA3\xBF\xE2\xA3\x80", "abc def ghi", "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"};
    char tmp[32];
    out = Str();
    for (int i = 0; i < n; ++i) {
        switch (rnd() % 26) {
            case 0: out += "\n"; break;
            case 1: out += "\r"; break;
            case 2: snprintf(tmp, sizeof(tmp), "\x1b[%um", 30 + rnd() % 8); out += tmp; break;
            case 3: out += "\x1b[0m"; break;
            case 4: out += "\x1b[1m"; break;
            case 5: out += "\x1b[7m"; break;
            case 6: snprintf(tmp, sizeof(tmp), "\x1b[%u;%uH", 1 + rnd() % 20, 1 + rnd() % 60); out += tmp; break;
            case 7: snprintf(tmp, sizeof(tmp), "\x1b[%uJ", rnd() % 3); out += tmp; break;
            case 8: snprintf(tmp, sizeof(tmp), "\x1b[%uK", rnd() % 3); out += tmp; break;
            case 9: snprintf(tmp, sizeof(tmp), "\x1b[%uL", 1 + rnd() % 3); out += tmp; break;
            case 10: snprintf(tmp, sizeof(tmp), "\x1b[%uM", 1 + rnd() % 3); out += tmp; break;
            case 11: out += "\x1b[4m"; break;
            case 12: snprintf(tmp, sizeof(tmp), "\x1b[%uS", 1 + rnd() % 4); out += tmp; break;
            case 13: out += "\b"; break;
            case 14: out += "\t"; break;
            case 15: snprintf(tmp, sizeof(tmp), "\x1b[%u@", 1 + rnd() % 4); out += tmp; break;
            case 16: snprintf(tmp, sizeof(tmp), "\x1b[%uP", 1 + rnd() % 4); out += tmp; break;
            case 17: snprintf(tmp, sizeof(tmp), "\x1b[%uX", 1 + rnd() % 4); out += tmp; break;
            case 18: snprintf(tmp, sizeof(tmp), "\x1b[%uC", 1 + rnd() % 5); out += tmp; break;
            case 19: snprintf(tmp, sizeof(tmp), "\x1b[%uD", 1 + rnd() % 5); out += tmp; break;
            case 20: snprintf(tmp, sizeof(tmp), "\x1b[%uA", 1 + rnd() % 3); out += tmp; break;
            case 21: out += "\x1b[?25l"; break;
            case 22: case 23: case 24: out += "\x1b[?25h"; break;
            default: out += words[rnd() % 9]; break;
        }
    }
}

static void render_tests() {
    Screen s(800, 480, true);
    corona::Metrics m = corona::metrics();
    expect(m.cell_w == 9 && m.cell_h == 20 && m.cols == 86 && m.rows == 23 && m.backbuffer, "corona: the small face and grid size for an 800x480 screen");

    u32 bg = 0x0A0A14;
    bool margin_clear = true;
    for (u32 x = 0; x < 800; ++x) if (s.at(x, 0) != bg) margin_clear = false;
    expect(margin_clear, "corona: the margins are filled with the void colour");

    corona::write("A");
    u32 ox = (800 - m.cols * 9) / 2, oy = (480 - m.rows * 20) / 2;
    int ink = 0;
    for (u32 y = 0; y < 20; ++y)
        for (u32 x = 0; x < 9; ++x)
            if (s.at(ox + x, oy + y) != bg) ++ink;
    expect(ink > 12 && ink < 160, "corona: a glyph puts ink in its cell");
    expect(s.at(ox + 9 + 1, oy + 2) == 0xFFB347, "corona: the cursor is a filled amber block on the next cell");
    corona::write("\x1b[2J\x1b[H");

    expect(corona::selftest(), "corona: the boot self-test passes");

    unsigned seed = 12345;
    bool all_equal = true;
    for (int trial = 0; trial < 40 && all_equal; ++trial) {
        Str stream;
        random_stream(stream, seed, 60 + trial * 7);
        corona::write(stream.c_str(), static_cast<u32>(stream.size()));
        Vec<u32> incremental = s.visible();
        Vec<u32> redraw = full_redraw_snapshot(s);
        if (incremental != redraw) {
            printf("  first difference in trial %d\n", trial);
            all_equal = false;
        }
    }
    expect(all_equal, "corona: incremental rendering with pixel scrolling equals a full redraw");

    Vec<u32> shown = s.visible();
    bool back_matches = true;
    for (size_t i = 0; i < shown.size(); ++i) if (shown[i] != s.back[i]) { back_matches = false; break; }
    expect(back_matches, "corona: the screen shows exactly what the backbuffer holds");

    corona::write("\x1b[2J\x1b[H\x1b[?25h");
    {
        unsigned seed3 = 4242;
        bool eq = true;
        for (int trial = 0; trial < 30 && eq; ++trial) {
            Str stream;
            random_stream(stream, seed3, 90);
            const char* d = stream.c_str();
            for (size_t i = 0; i < stream.size() && eq;) {
                size_t j = i + 1;
                if (d[i] == 0x1b) while (j < stream.size() && !(d[j] >= 0x40 && d[j] <= 0x7E && j > i + 1)) ++j;
                if (j < stream.size() && d[i] == 0x1b) ++j;
                else if ((static_cast<unsigned char>(d[i]) & 0xC0) == 0xC0)
                    while (j < stream.size() && (static_cast<unsigned char>(d[j]) & 0xC0) == 0x80) ++j;
                corona::write(stream.c_str() + i, static_cast<u32>(j - i));
                i = j;
                {
                    Vec<u32> inc = s.visible();
                    if (inc != full_redraw_snapshot(s)) {
                        printf("  difference in typing trial %d after:", trial);
                        for (size_t q = (i > 12 ? i - 12 : 0); q < i; ++q) printf(" %02x", static_cast<unsigned char>(d[q]));
                        printf("\n");
                        eq = false;
                    }
                }
            }
        }
        expect(eq, "corona: key-by-key editing equals a full redraw at every step");
    }
    corona::write("\x1b[?25h\x1b[2J\x1b[H");
    for (int i = 0; i < 400; ++i) {
        char line[64];
        snprintf(line, sizeof(line), "line %d \xE2\x95\x91 \xCE\xA9\n", i);
        corona::write(line);
    }
    Vec<u32> after = s.visible();
    Vec<u32> redraw = full_redraw_snapshot(s);
    expect(after == redraw, "corona: a long scrolling run ends identical to a full redraw");

    {
        Screen direct(800, 480, false, 16);
        unsigned seed2 = 777;
        bool eq = true;
        for (int trial = 0; trial < 25 && eq; ++trial) {
            Str stream;
            random_stream(stream, seed2, 80);
            corona::write(stream.c_str(), static_cast<u32>(stream.size()));
            Vec<u32> inc = direct.visible();
            Vec<u32> full = full_redraw_snapshot(direct);
            if (inc != full) eq = false;
        }
        expect(eq, "corona: direct mode with a padded pitch equals a full redraw");
        corona::write("\x1b[2J\x1b[H");
        corona::write("x");
        expect(corona::selftest() == false, "corona: the self-test refuses to run away from the start of a line");
    }

    {
        Screen big(1920, 1200, true);
        corona::Metrics bm = corona::metrics();
        expect(bm.cell_w == 11 && bm.cell_h == 24 && bm.cols == 172 && bm.rows == 49, "corona: the medium face for a 1920x1200 panel");
        corona::write("\x1b[1;38;2;255;179;71mAphelion\x1b[0m \xE2\x82\xB9\n");
        expect(corona::selftest(), "corona: the self-test passes on the medium face");
    }
    {
        Screen huge(3840, 2160, true);
        corona::Metrics hm = corona::metrics();
        expect(hm.cell_w == 14 && hm.cell_h == 31 && hm.cols <= term::MAX_COLS && hm.rows <= term::MAX_ROWS, "corona: the large face for a 4K panel stays within the grid limits");
    }
    {
        fb::Info bad{};
        Vec<u32> mem;
        mem.assign(64 * 64, 0);
        bad.base = reinterpret_cast<volatile u8*>(mem.data());
        bad.width = 64; bad.height = 64; bad.pitch = 256; bad.bpp = 24;
        expect(!corona::init(bad, g_blob.data(), g_blob.size()), "corona: a framebuffer that is not 32 bits per pixel is refused");
        bad.bpp = 32; bad.pitch = 258;
        expect(!corona::init(bad, g_blob.data(), g_blob.size()), "corona: an unaligned pitch is refused");
    }
}

static void scrollback_render_tests() {
    Screen s(800, 480, true);
    Vec<term::Cell> mem;
    mem.assign(static_cast<size_t>(200) * 86, term::Cell{});
    u32 lines = corona::enable_scrollback(mem.data(), mem.size() * sizeof(term::Cell));
    expect(lines == 200, "corona: scrollback sizes itself from the memory it is given");
    expect(corona::scrollback_bytes_per_line() == 86 * sizeof(term::Cell), "corona: one history line costs one row of cells");

    for (int i = 0; i < 100; ++i) {
        char line[64];
        snprintf(line, sizeof(line), "scroll line %d\n", i);
        corona::write(line);
    }
    expect(corona::scrollback_lines() == 78 && corona::view_offset() == 0, "corona: lines that scrolled away are counted");
    Vec<u32> live = s.visible();
    u32 amber_live = 0;
    for (size_t i = 0; i < live.size(); ++i) if (live[i] == 0xFFB347) ++amber_live;
    expect(amber_live > 0, "corona: the cursor shows at the bottom");

    bool steps_equal = true;
    const i32 moves[] = {1, 5, 21, 30, -3, -40, 100, 2, -1, -200};
    for (i32 mv : moves) {
        corona::scroll_view(mv);
        Vec<u32> inc = s.visible();
        if (inc != full_redraw_snapshot(s)) { printf("  view differs after moving %d\n", mv); steps_equal = false; }
    }
    expect(steps_equal, "corona: every scroll step equals a full redraw");
    expect(corona::view_offset() == 0 && s.visible() == live, "corona: scrolling back to the bottom restores the exact live screen");

    corona::scroll_view(10);
    Vec<u32> back = s.visible();
    u32 amber_back = 0;
    for (size_t i = 0; i < back.size(); ++i) if (back[i] == 0xFFB347) ++amber_back;
    expect(back != live && amber_back == 0, "corona: the view differs from the live screen and the cursor is hidden");
    bool matches = true;
    for (size_t i = 0; i < back.size(); ++i) if (back[i] != s.back[i]) { matches = false; break; }
    expect(matches, "corona: the screen shows what the backbuffer holds while scrolled back");

    corona::write("x");
    expect(corona::view_offset() == 0, "corona: output snaps the view to the bottom");
    Vec<u32> after = s.visible();
    expect(after == full_redraw_snapshot(s), "corona: the snapped screen equals a full redraw");

    expect(!corona::scroll_view(-1) && corona::snap_to_live() == false, "corona: nothing to do at the bottom");
    corona::scroll_view(7);
    expect(corona::snap_to_live() && corona::view_offset() == 0, "corona: snap_to_live returns to the bottom");

    unsigned seed = 99;
    bool eq = true;
    for (int trial = 0; trial < 30 && eq; ++trial) {
        Str stream;
        random_stream(stream, seed, 80);
        corona::write(stream.c_str(), static_cast<u32>(stream.size()));
        if (trial % 3 == 0) corona::scroll_view(1 + trial);
        if (s.visible() != full_redraw_snapshot(s)) { printf("  scrollback trial %d differs\n", trial); eq = false; }
    }
    expect(eq, "corona: random output with scrolling and scrollback equals a full redraw");

    corona::write("\x1b[0m\x1b[r\x1b[?7h\x1b[2J\x1b[H\x1b[?25h");
    corona::write("\x1b[41m  \x1b[0mX");
    const u32 ox = (800 - 86 * 9) / 2, oy = (480 - 23 * 20) / 2;
    bool red = true;
    for (u32 y = 0; y < 20; ++y)
        for (u32 x = 0; x < 18; ++x)
            if (s.at(ox + x, oy + y) != 0xFF6B7A) red = false;
    expect(red, "corona: a run of coloured blanks fills exactly its cells");
    expect(s.at(ox + 36, oy + 0) == 0x0A0A14 && s.at(ox - 1, oy + 5) == 0x0A0A14, "corona: and nothing around it");
    expect(s.visible() == full_redraw_snapshot(s), "corona: the run survives a full redraw");
}

static int ink_of(const u8* g, u32 n) {
    int total = 0;
    for (u32 i = 0; i < n; ++i) total += corona::font::coverage(g, i);
    return total;
}

static void font_tests() {
    using namespace corona::font;
    expect(load(g_blob.data(), g_blob.size()) && face_count() == 3, "font: the baked blob loads with three faces");
    expect(!load(g_blob.data(), 200), "font: a truncated blob is refused");
    Vec<u8> corrupt = g_blob;
    corrupt[0] = 'X';
    expect(!load(corrupt.data(), corrupt.size()), "font: a blob with the wrong magic is refused");
    Vec<u8> bad_count = g_blob;
    bad_count[8] = 9;
    expect(!load(bad_count.data(), bad_count.size()), "font: an impossible face count is refused");
    Vec<u8> bad_off = g_blob;
    bad_off[12 + 16] = 0xFF; bad_off[12 + 17] = 0xFF; bad_off[12 + 18] = 0xFF; bad_off[12 + 19] = 0x7F;
    expect(!load(bad_off.data(), bad_off.size()), "font: a table that points past the end is refused");
    expect(load(g_blob.data(), g_blob.size()), "font: the intact blob loads again after the failures");

    expect(pick(1280, 800) == 0 && pick(1920, 1200) == 1 && pick(1920, 1080) == 1 && pick(3840, 2160) == 2 && pick(800, 600) == 0, "font: face choice by screen size");

    bool all_cover = true;
    for (u32 i = 0; i < face_count(); ++i) {
        const Face& f = face(i);
        const u32 must[] = {0x20, 'A', 'z', 0xE9, 0x3A9, 0x416, 0x20AC, 0x20B9, 0x2500, 0x2554, 0x256D, 0x2588, 0x2591, 0x2800, 0x28FF, 0x25CF, 0x2713, 0x2192, 0xFFFD, 0x1EA0, 0x2264};
        for (u32 cp : must) if (!has(f, cp)) all_cover = false;
    }
    expect(all_cover, "font: every face covers Latin, Greek, Cyrillic, the rupee sign, boxes, blocks, braille and arrows");

    const Face& f = face(1);
    u32 n = f.cell_w * f.cell_h;
    bool fb1 = false, fb2 = false;
    const u8* ga = glyph(f, 'A', false, &fb1);
    const u8* gx = glyph(f, 0x4E2D, false, &fb2);
    expect(!fb1 && fb2 && ink_of(ga, n) > 0, "font: known characters use their glyph, unknown ones the replacement");
    expect(ink_of(glyph(f, 0x20, false, nullptr), n) == 0, "font: the space glyph is empty");
    expect(ink_of(gx, n) == ink_of(glyph(f, 0xFFFD, false, nullptr), n), "font: unknown characters draw the replacement glyph");
    expect(ink_of(glyph(f, 'A', true, nullptr), n) > ink_of(ga, n), "font: bold has more ink than regular");
    expect(ink_of(glyph(f, 0x2588, false, nullptr), n) == static_cast<int>(n) * 15, "font: the full block is solid");

    bool connected = true;
    for (u32 i = 0; i < face_count(); ++i) {
        const Face& ff = face(i);
        const u8* h = glyph(ff, 0x2500, false, nullptr);
        const u8* v = glyph(ff, 0x2502, false, nullptr);
        for (u32 x = 0; x < ff.cell_w; ++x) {
            bool any = false;
            for (u32 y = 0; y < ff.cell_h; ++y) if (coverage(h, y * ff.cell_w + x)) any = true;
            if (!any) connected = false;
        }
        for (u32 y = 0; y < ff.cell_h; ++y) {
            bool any = false;
            for (u32 x = 0; x < ff.cell_w; ++x) if (coverage(v, y * ff.cell_w + x)) any = true;
            if (!any) connected = false;
        }
    }
    expect(connected, "font: horizontal and vertical box lines reach every edge so neighbours join");

    bool reaches = true;
    for (u32 i = 0; i < face_count(); ++i) {
        const Face& ff = face(i);
        const u8* c = glyph(ff, 0x253C, false, nullptr);
        u32 top = 0, bottom = 0, left = 0, right = 0;
        for (u32 x = 0; x < ff.cell_w; ++x) {
            top += coverage(c, x);
            bottom += coverage(c, (ff.cell_h - 1) * ff.cell_w + x);
        }
        for (u32 y = 0; y < ff.cell_h; ++y) {
            left += coverage(c, y * ff.cell_w);
            right += coverage(c, y * ff.cell_w + ff.cell_w - 1);
        }
        if (!top || !bottom || !left || !right) reaches = false;
    }
    expect(reaches, "font: the cross reaches all four edges");

    const u8* braille_full = glyph(f, 0x28FF, false, nullptr);
    const u8* braille_one = glyph(f, 0x2801, false, nullptr);
    expect(ink_of(braille_full, n) > ink_of(braille_one, n) * 4 && ink_of(braille_one, n) > 0, "font: braille dot count scales the ink");
    bool all_unique = true;
    for (u32 a = 0x2800; a < 0x2900 && all_unique; a += 17)
        for (u32 b = a + 1; b < 0x2900; b += 23)
            if (mem::compare(glyph(f, a, false, nullptr), glyph(f, b, false, nullptr), n / 2) == 0) all_unique = false;
    expect(all_unique, "font: distinct braille patterns have distinct glyphs");
    expect(ink_of(glyph(f, 0x20B9, false, nullptr), n) > ink_of(glyph(f, '-', false, nullptr), n), "font: the rupee sign has more ink than a hyphen");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (!load_blob()) {
        printf("cannot read kernel/data/corona-fonts.bin (run from the repository root)\n");
        return 2;
    }
    utf8_tests();
    font_tests();
    term_tests();
    render_tests();
    history_tests();
    scrollback_render_tests();
    return g_failures ? 1 : 0;
}
