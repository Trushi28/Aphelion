#include <cosmos/helm_rows.hpp>
#include <cosmos/kheap.hpp>

namespace helm {

namespace {

using rows::Kind;

int usage(Io& io, const char* text) {
    print_error(io, text);
    return 2;
}

bool next_line(const u8* data, u64 size, u64* pos, u64* start, u64* len) {
    if (*pos >= size) return false;
    *start = *pos;
    u64 i = *pos;
    while (i < size && data[i] != '\n') ++i;
    *len = i - *start;
    *pos = i < size ? i + 1 : i;
    return true;
}

char fold(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

bool find_in(const u8* hay, u64 hay_len, const char* needle, u32 needle_len, bool icase) {
    if (needle_len == 0) return true;
    if (needle_len > hay_len) return false;
    for (u64 i = 0; i + needle_len <= hay_len; ++i) {
        u32 k = 0;
        for (; k < needle_len; ++k) {
            char a = static_cast<char>(hay[i + k]);
            char b = needle[k];
            if (icase) { a = fold(a); b = fold(b); }
            if (a != b) break;
        }
        if (k == needle_len) return true;
    }
    return false;
}

struct SeekCtx {
    Shell* sh;
    rows::Table* out;
    const char* needle;
    u32 needle_len;
    bool fold_case;
    bool invert;
    bool count_only;
    bool show_file;
    u64 matches;
    bool failed;
};

void seek_emit(const char* name, const u8* data, u64 size, void* raw) {
    SeekCtx& c = *static_cast<SeekCtx*>(raw);
    if (c.failed) return;
    u64 pos = 0, s, l, n = 0, hits = 0;
    while (next_line(data, size, &pos, &s, &l)) {
        ++n;
        if (c.sh->poll()) return;
        bool found = find_in(data + s, l, c.needle, c.needle_len, c.fold_case);
        if (found == c.invert) continue;
        ++hits;
        ++c.matches;
        if (c.count_only) continue;
        u32 row = rows::add_row(*c.out);
        if (row == ~0u) { c.failed = true; return; }
        u32 col = 0;
        if (c.show_file) rows::set_text(*c.out, row, col++, name ? name : "input");
        rows::set_num(*c.out, row, col++, n);
        rows::set_text(*c.out, row, col, reinterpret_cast<const char*>(data) + s, static_cast<u32>(l));
        if (c.out->failed) { c.failed = true; return; }
    }
    if (c.count_only) {
        u32 row = rows::add_row(*c.out);
        if (row == ~0u) { c.failed = true; return; }
        u32 col = 0;
        if (c.show_file) rows::set_text(*c.out, row, col++, name ? name : "input");
        rows::set_num(*c.out, row, col, hits);
    }
}

int cmd_seek(Shell& sh, Io& io, u32 argc, char** argv) {
    bool fold_case = false, invert = false, count_only = false;
    if (!opt_bool(io, "fold", &fold_case) || !opt_bool(io, "not", &invert) || !opt_bool(io, "count", &count_only)) return 2;
    if (argc < 2) return usage(io, "usage: seek text [file...]  fold: yes  not: yes  count: yes   (plain text, not a pattern; fold ignores ASCII case)");
    SeekCtx c{};
    c.sh = &sh;
    c.out = io.out_rows;
    c.needle = argv[1];
    c.needle_len = static_cast<u32>(cstr::length(argv[1]));
    c.fold_case = fold_case;
    c.invert = invert;
    c.count_only = count_only;
    c.show_file = argc > 3;
    rows::Table& out = *io.out_rows;
    rows::begin(out);
    if (c.show_file) rows::add_column(out, "file", Kind::Text);
    if (count_only) {
        rows::add_column(out, "matches", Kind::Int);
    } else {
        rows::add_column(out, "line", Kind::Int);
        rows::add_column(out, "text", Kind::Text);
        out.body = static_cast<i32>(out.cols) - 1;
    }
    int st = for_each_input(sh, io, argc, argv, 2, &seek_emit, &c);
    if (sh.aborted) return 130;
    if (c.failed || out.failed) { print_error(io, "too many matches or out of memory"); return 1; }
    if (st != 0) return st;
    return c.matches ? 0 : 1;
}

struct PrismCtx {
    Shell* sh;
    Io* io;
    u64 skip;
    u64 limit;
    bool limited;
};

void prism_emit(const char*, const u8* data, u64 size, void* raw) {
    PrismCtx& c = *static_cast<PrismCtx*>(raw);
    Writer& w = c.io->out;
    if (c.skip >= size) return;
    u64 end = size;
    if (c.limited && c.skip + c.limit < end) end = c.skip + c.limit;
    for (u64 off = c.skip; off < end; off += 16) {
        if (c.sh->poll()) return;
        w.rgb(palette::MIST);
        w.hex(off, 8);
        w.str("  ");
        w.reset();
        u64 row = end - off < 16 ? end - off : 16;
        for (u64 i = 0; i < 16; ++i) {
            if (i < row) { w.hex(data[off + i], 2); } else { w.str("  "); }
            w.put(' ');
            if (i == 7) w.put(' ');
        }
        w.str(" ");
        w.rgb(palette::ICE);
        for (u64 i = 0; i < row; ++i) {
            u8 b = data[off + i];
            w.put(b >= 0x20 && b < 0x7F ? static_cast<char>(b) : '.');
        }
        w.reset();
        w.put('\n');
    }
}

int cmd_prism(Shell& sh, Io& io, u32 argc, char** argv) {
    PrismCtx c{&sh, &io, 0, 0, false};
    if (!opt_num(io, "from", &c.skip)) return 2;
    if (opt(io, "take")) {
        if (!opt_num(io, "take", &c.limit)) return 2;
        c.limited = true;
    }
    if (argc > 2) return usage(io, "usage: prism [file]  from: OFFSET  take: BYTES");
    int st = for_each_input(sh, io, argc, argv, 1, &prism_emit, &c);
    return sh.aborted ? 130 : st;
}

constexpr Command COMMANDS[] = {
    {"seek", "seek text [file...]  fold: yes  not: yes  count: yes", "find lines containing text, as rows: line and text", &cmd_seek, "fold not count"},
    {"prism", "prism [file]  from: OFFSET  take: BYTES", "hex and character view of bytes", &cmd_prism, "from take"},
};

constexpr Table TABLE{"text", COMMANDS, sizeof(COMMANDS) / sizeof(COMMANDS[0])};

}

void register_text_commands() { register_table(&TABLE); }

}
