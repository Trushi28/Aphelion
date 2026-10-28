#include <cosmos/helm_rows.hpp>
#include <cosmos/nebula_realms.hpp>
#include <cosmos/corona.hpp>
#include <cosmos/input.hpp>
#include <cosmos/orbital.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/kheap.hpp>

namespace helm {

namespace {

constexpr u32 MAX_LINE = 256;
constexpr u32 LINE_BYTES = MAX_LINE * 4 + 1;
constexpr u32 HISTORY = 64;
constexpr u32 PROMPT_PATH_CELLS = 28;

void terminal_write(void*, const char* data, u32 n) {
    corona::write(data, n);
    serial::console_write(data, n);
}

bool poll_interrupt(void*) {
    input::Key k;
    bool hit = false;
    while (input::try_read(k))
        if (k.sym == input::Sym::None && k.ctrl() && k.cp == 'c') hit = true;
    return hit;
}

struct Line {
    u32 cp[MAX_LINE];
    u32 len;
    u32 pos;
    u32 view;

    u32 width_of(u32 from, u32 to) const {
        u32 w = 0;
        for (u32 i = from; i < to; ++i) w += utf8::width(cp[i]);
        return w;
    }
    bool insert(u32 c) {
        if (len >= MAX_LINE) return false;
        for (u32 i = len; i > pos; --i) cp[i] = cp[i - 1];
        cp[pos++] = c;
        ++len;
        return true;
    }
    void erase_range(u32 from, u32 to) {
        if (from >= to) return;
        u32 n = to - from;
        for (u32 i = to; i < len; ++i) cp[i - n] = cp[i];
        len -= n;
        pos = from;
    }
    void erase_before() { if (pos) erase_range(pos - 1, pos); }
    void erase_at() { if (pos < len) erase_range(pos, pos + 1); }
    u32 encode_range(u32 from, u32 to, char* out, u32 max) const {
        u32 n = 0;
        for (u32 i = from; i < to; ++i) {
            char e[4];
            u32 k = utf8::encode(cp[i], e);
            if (n + k >= max) break;
            for (u32 j = 0; j < k; ++j) out[n++] = e[j];
        }
        out[n] = 0;
        return n;
    }
    u32 encode(char* out, u32 max) const { return encode_range(0, len, out, max); }
    void set(const char* s, u32 n) {
        len = pos = view = 0;
        utf8::Decoder d;
        u32 tmp[2];
        for (u32 i = 0; i < n; ++i) {
            u32 k = d.feed(static_cast<u8>(s[i]), tmp);
            for (u32 j = 0; j < k; ++j) insert(tmp[j]);
        }
        u32 tail[1];
        if (d.finish(tail)) insert(tail[0]);
    }
    void clear() { len = pos = view = 0; }
    bool is_word_char(u32 c) const { return c != ' ' && c != '\t' && c != '/' && c != ':' && c != '|' && c != ';'; }
    u32 word_left() const {
        u32 i = pos;
        while (i > 0 && !is_word_char(cp[i - 1])) --i;
        while (i > 0 && is_word_char(cp[i - 1])) --i;
        return i;
    }
    u32 word_right() const {
        u32 i = pos;
        while (i < len && !is_word_char(cp[i])) ++i;
        while (i < len && is_word_char(cp[i])) ++i;
        return i;
    }
};

struct Editor {
    Line line;
    char history[HISTORY][LINE_BYTES];
    u32 history_count;
    u32 history_next;
    u32 history_back;
    char saved[LINE_BYTES];
    bool want_list;
    char prompt_path[nebula::path::TEXT_MAX];
    u32 prompt_cells;
};

Editor* g_editor;

bool same_text(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

void remember(Editor& e, const char* s, u32 n) {
    if (!n) return;
    u32 last = (e.history_next + HISTORY - 1) % HISTORY;
    if (e.history_count && same_text(e.history[last], s)) return;
    mem::copy(e.history[e.history_next], s, n + 1);
    e.history_next = (e.history_next + 1) % HISTORY;
    if (e.history_count < HISTORY) ++e.history_count;
}

const char* recall(const Editor& e, u32 back) {
    if (back == 0 || back > e.history_count) return nullptr;
    return e.history[(e.history_next + HISTORY - back) % HISTORY];
}

void build_prompt(Shell& sh, Editor& e) {
    char loc[nebula::path::TEXT_MAX];
    lens::text::location_text(sh.cursor, loc, sizeof(loc));
    u32 n = static_cast<u32>(cstr::length(loc));
    u32 cells = utf8::cells(loc, n);
    if (cells <= PROMPT_PATH_CELLS) {
        mem::copy(e.prompt_path, loc, n + 1);
    } else {
        u32 start = 0;
        for (u32 i = 0; i < n; ++i) {
            if ((static_cast<u8>(loc[i]) & 0xC0) == 0x80) continue;
            if (utf8::cells(loc + i, n - i) <= PROMPT_PATH_CELLS - 1) { start = i; break; }
        }
        e.prompt_path[0] = '\xE2';
        e.prompt_path[1] = '\x80';
        e.prompt_path[2] = '\xA6';
        mem::copy(e.prompt_path + 3, loc + start, n - start + 1);
    }
    e.prompt_cells = 8 + 1 + utf8::cells(e.prompt_path, static_cast<u32>(cstr::length(e.prompt_path))) + 3;
}

void draw_prompt(Writer& w, const Editor& e) {
    w.str("\r");
    w.rgb(palette::AMBER);
    w.str("aphelion");
    w.reset();
    w.put(' ');
    w.rgb(palette::MIST);
    w.str(e.prompt_path);
    w.rgb(palette::EMBER);
    w.str(" \xE2\x96\xB8 ");
    w.reset();
}

void redraw(Writer& w, Editor& e, u32 cols) {
    Line& l = e.line;
    u32 avail = cols > e.prompt_cells + 2 ? cols - e.prompt_cells - 1 : 8;
    if (l.pos < l.view) l.view = l.pos;
    while (l.view < l.pos && l.width_of(l.view, l.pos) + 1 > avail) ++l.view;
    draw_prompt(w, e);
    u32 used = 0;
    u32 end = l.view;
    while (end < l.len) {
        u32 cw = utf8::width(l.cp[end]);
        if (used + cw > avail) break;
        used += cw;
        w.cp(l.cp[end]);
        ++end;
    }
    w.str("\x1b[K");
    u32 back = l.width_of(l.pos, end);
    if (back) { w.str("\x1b["); w.num(back); w.put('D'); }
    w.flush();
}

void list_candidates(Writer& w, const Completion& c, u32 cols) {
    u32 widest = 0;
    const char* p = c.names;
    for (u32 i = 0; i < c.count; ++i) {
        u32 n = static_cast<u32>(cstr::length(p));
        u32 cells = utf8::cells(p, n);
        if (cells > widest) widest = cells;
        p += n + 1;
    }
    u32 col_w = widest + 2;
    u32 per_row = cols / col_w;
    if (per_row == 0) per_row = 1;
    u32 rows = (c.count + per_row - 1) / per_row;
    const char* items[128];
    p = c.names;
    for (u32 i = 0; i < c.count && i < 128; ++i) {
        items[i] = p;
        p += cstr::length(p) + 1;
    }
    w.put('\n');
    for (u32 r = 0; r < rows; ++r) {
        for (u32 col = 0; col < per_row; ++col) {
            u32 idx = col * rows + r;
            if (idx >= c.count) break;
            const char* name = items[idx];
            u32 n = static_cast<u32>(cstr::length(name));
            bool dir = n && name[n - 1] == '/';
            if (dir) w.rgb(palette::ICE);
            w.str(name);
            w.reset();
            if ((col + 1) * rows + r < c.count) w.spaces(col_w - utf8::cells(name, n));
        }
        w.put('\n');
    }
}

void handle_tab(Shell& sh, Writer& w, Editor& e, u32 cols) {
    Line& l = e.line;
    char text[LINE_BYTES];
    u32 total = l.encode(text, sizeof(text));
    char prefix[LINE_BYTES];
    u32 cursor = l.encode_range(0, l.pos, prefix, sizeof(prefix));
    Completion c;
    if (!complete(sh, text, cursor, &c)) { e.want_list = false; return; }
    if (e.want_list && c.count > 1) {
        list_candidates(w, c, cols);
        release(c);
        e.want_list = false;
        return;
    }
    char rebuilt[LINE_BYTES];
    u32 n = 0;
    u32 ins = static_cast<u32>(cstr::length(c.insert));
    if (c.start + ins + (total - cursor) + 1 < sizeof(rebuilt)) {
        mem::copy(rebuilt, text, c.start);
        n = c.start;
        mem::copy(rebuilt + n, c.insert, ins);
        n += ins;
        u32 new_cursor = n;
        mem::copy(rebuilt + n, text + cursor, total - cursor);
        n += total - cursor;
        rebuilt[n] = 0;
        l.set(rebuilt, n);
        u32 pos_cp = 0;
        utf8::Decoder d;
        u32 tmp[2];
        for (u32 i = 0; i < new_cursor; ++i) pos_cp += d.feed(static_cast<u8>(rebuilt[i]), tmp);
        l.pos = pos_cp > l.len ? l.len : pos_cp;
    }
    e.want_list = c.count > 1;
    release(c);
}

void loop(void*) {
    Editor* e = static_cast<Editor*>(kheap::alloc_zeroed(sizeof(Editor)));
    Shell* sh = static_cast<Shell*>(kheap::alloc_zeroed(sizeof(Shell)));
    if (!e || !sh) {
        serial::writeln("[helm] out of memory");
        return;
    }
    g_editor = e;
    corona::Metrics m = corona::metrics();
    const char* start = nebula::disk_is_mounted() ? "disk" : "ram";
    init(*sh, Sink{&terminal_write, nullptr}, true, m.cols, start);
    sh->interrupt_poll = &poll_interrupt;

    Writer w(Sink{&terminal_write, nullptr}, true, true);
    print_splash(w);
    build_prompt(*sh, *e);
    redraw(w, *e, m.cols);

    Line& l = e->line;
    for (;;) {
        input::Key k;
        input::read(k);
        bool edited = true;
        bool tab = false;

        if (k.sym == input::Sym::Enter) {
            redraw(w, *e, m.cols);
            w.put('\n');
            w.flush();
            char text[LINE_BYTES];
            u32 n = l.encode(text, sizeof(text));
            remember(*e, text, n);
            l.clear();
            e->history_back = 0;
            e->want_list = false;
            run_line(*sh, text);
            w.flush();
            if (sh->quit) return;
            m = corona::metrics();
            sh->cols = m.cols;
            build_prompt(*sh, *e);
            redraw(w, *e, m.cols);
            continue;
        }
        if (k.sym == input::Sym::PageUp || k.sym == input::Sym::PageDown ||
            (k.shift() && (k.sym == input::Sym::Up || k.sym == input::Sym::Down))) {
            const bool back = k.sym == input::Sym::PageUp || k.sym == input::Sym::Up;
            const i32 step = k.sym == input::Sym::PageUp || k.sym == input::Sym::PageDown
                ? static_cast<i32>(m.rows > 2 ? m.rows - 2 : 1) : 1;
            corona::scroll_view(back ? step : -step);
            continue;
        }
        if (k.sym == input::Sym::Tab) {
            tab = true;
            handle_tab(*sh, w, *e, m.cols);
        } else if (k.sym == input::Sym::Backspace) l.erase_before();
        else if (k.sym == input::Sym::Delete) l.erase_at();
        else if (k.sym == input::Sym::Left) { l.pos = k.ctrl() ? l.word_left() : (l.pos ? l.pos - 1 : 0); }
        else if (k.sym == input::Sym::Right) { l.pos = k.ctrl() ? l.word_right() : (l.pos < l.len ? l.pos + 1 : l.len); }
        else if (k.sym == input::Sym::Home) l.pos = 0;
        else if (k.sym == input::Sym::End) l.pos = l.len;
        else if (k.sym == input::Sym::Up) {
            const char* h = recall(*e, e->history_back + 1);
            if (h) {
                if (e->history_back == 0) l.encode(e->saved, sizeof(e->saved));
                ++e->history_back;
                l.set(h, static_cast<u32>(cstr::length(h)));
            }
        } else if (k.sym == input::Sym::Down) {
            if (e->history_back > 0) {
                --e->history_back;
                const char* h = e->history_back ? recall(*e, e->history_back) : e->saved;
                l.set(h, static_cast<u32>(cstr::length(h)));
            }
        } else if (k.sym == input::Sym::None && k.ctrl()) {
            if (k.cp == 'c') {
                redraw(w, *e, m.cols);
                w.str("^C\n");
                l.clear();
                e->history_back = 0;
            } else if (k.cp == 'l') {
                w.str("\x1b[2J\x1b[H");
            } else if (k.cp == 'a') l.pos = 0;
            else if (k.cp == 'e') l.pos = l.len;
            else if (k.cp == 'u') l.erase_range(0, l.pos);
            else if (k.cp == 'k') l.len = l.pos;
            else if (k.cp == 'w') l.erase_range(l.word_left(), l.pos);
            else if (k.cp == 'd' && l.len == 0) {
                w.str("\n  use ");
                w.rgb(palette::ICE);
                w.str("sunset");
                w.reset();
                w.str(" or ");
                w.rgb(palette::ICE);
                w.str("restart");
                w.reset();
                w.put('\n');
            } else edited = false;
        } else if (k.is_text() && !k.alt() && k.cp >= 0x20) {
            l.insert(k.cp);
        } else {
            edited = false;
        }
        if (!tab) e->want_list = false;
        if (edited) {
            if (k.sym == input::Sym::Up || k.sym == input::Sym::Down) l.pos = l.len;
            redraw(w, *e, m.cols);
        }
    }
}

int cmd_recall(Shell&, Io& io, u32, char**) {
    Editor* e = g_editor;
    if (!e) return 0;
    rows::Table& t = *io.out_rows;
    rows::begin(t);
    rows::add_column(t, "n", rows::Kind::Int);
    rows::add_column(t, "command", rows::Kind::Text);
    t.body = 1;
    for (u32 i = e->history_count; i >= 1; --i) {
        u32 r = rows::add_row(t);
        if (r == ~0u) break;
        rows::set_num(t, r, 0, e->history_count - i + 1);
        rows::set_text(t, r, 1, recall(*e, i));
    }
    return 0;
}

constexpr Command COMMANDS[] = {
    {"recall", "recall", "the commands typed in this shell, as rows", &cmd_recall},
};

constexpr Table TABLE{"editor", COMMANDS, sizeof(COMMANDS) / sizeof(COMMANDS[0])};

}

bool spawn() {
    register_builtin_commands();
    register_fs_commands();
    register_text_commands();
    register_row_commands();
    register_sys_commands();
    register_table(&TABLE);
    return orbital::spawn("helm", &loop, nullptr) != nullptr;
}

}
