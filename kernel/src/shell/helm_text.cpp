#include <cosmos/helm.hpp>
#include <cosmos/kheap.hpp>

namespace helm {

namespace {

u32 length(const char* s) { return static_cast<u32>(__builtin_strlen(s)); }

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

u64 line_count(const u8* data, u64 size) {
    u64 n = 0, pos = 0, s, l;
    while (next_line(data, size, &pos, &s, &l)) ++n;
    return n;
}

void put_line(Writer& w, const u8* data, u64 start, u64 len) {
    w.bytes(reinterpret_cast<const char*>(data) + start, len);
    w.put('\n');
}

struct NameCtx {
    Shell* sh;
    Io* io;
    u64 count;
    bool show_names;
    u32 files;
    u32 seen;
    u64 numeric;
    bool flag_a, flag_b, flag_c, flag_d;
    const char* needle;
    u32 needle_len;
    u64 matches;
    u64 total_lines, total_words, total_bytes, total_cps;
};

void banner(NameCtx& c, const char* name) {
    if (!c.show_names) return;
    Writer& w = c.io->out;
    if (c.seen) w.put('\n');
    w.rgb(palette::AMBER);
    w.str("==> ");
    w.str(name ? name : "stdin");
    w.str(" <==");
    w.reset();
    w.put('\n');
    ++c.seen;
}

void head_emit(const char* name, const u8* data, u64 size, void* raw) {
    NameCtx& c = *static_cast<NameCtx*>(raw);
    banner(c, name);
    u64 pos = 0, s, l, n = 0;
    while (n < c.count && next_line(data, size, &pos, &s, &l)) {
        if (c.sh->poll()) return;
        put_line(c.io->out, data, s, l);
        ++n;
    }
}

void tail_emit(const char* name, const u8* data, u64 size, void* raw) {
    NameCtx& c = *static_cast<NameCtx*>(raw);
    banner(c, name);
    u64 total = line_count(data, size);
    u64 skip = total > c.count ? total - c.count : 0;
    u64 pos = 0, s, l, n = 0;
    while (next_line(data, size, &pos, &s, &l)) {
        if (c.sh->poll()) return;
        if (n++ < skip) continue;
        put_line(c.io->out, data, s, l);
    }
}

int line_count_option(Shell& sh, Io& io, u32 argc, char** argv, void (*fn)(const char*, const u8*, u64, void*), const char* use) {
    Options o = options_begin(argc, argv);
    NameCtx c{};
    c.sh = &sh;
    c.io = &io;
    c.count = 10;
    int opt;
    while ((opt = next_option(o, "", "n")) >= 0) {
        if (opt == 'n' && (!parse_u64(o.value, &c.count))) return usage(io, use);
    }
    if (o.bad) return usage(io, use);
    c.show_names = argc - o.index > 1;
    int st = for_each_input(sh, io, o, fn, &c);
    return sh.aborted ? 130 : st;
}

int cmd_head(Shell& sh, Io& io, u32 argc, char** argv) {
    return line_count_option(sh, io, argc, argv, &head_emit, "usage: head [-n lines] [file...]");
}

int cmd_tail(Shell& sh, Io& io, u32 argc, char** argv) {
    return line_count_option(sh, io, argc, argv, &tail_emit, "usage: tail [-n lines] [file...]");
}

char fold(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

bool find_in(const u8* hay, u64 hay_len, const char* needle, u32 needle_len, bool icase, u64* at) {
    if (needle_len == 0) { *at = 0; return true; }
    if (needle_len > hay_len) return false;
    for (u64 i = 0; i + needle_len <= hay_len; ++i) {
        u32 k = 0;
        for (; k < needle_len; ++k) {
            char a = static_cast<char>(hay[i + k]);
            char b = needle[k];
            if (icase) { a = fold(a); b = fold(b); }
            if (a != b) break;
        }
        if (k == needle_len) { *at = i; return true; }
    }
    return false;
}

void grep_emit(const char* name, const u8* data, u64 size, void* raw) {
    NameCtx& c = *static_cast<NameCtx*>(raw);
    Writer& w = c.io->out;
    bool icase = c.flag_a, invert = c.flag_b, only_count = c.flag_c, numbered = c.flag_d;
    u64 pos = 0, s, l, n = 0, hits = 0;
    while (next_line(data, size, &pos, &s, &l)) {
        ++n;
        if (c.sh->poll()) return;
        u64 at = 0;
        bool found = find_in(data + s, l, c.needle, c.needle_len, icase, &at);
        if (found == invert) continue;
        ++hits;
        ++c.matches;
        if (only_count) continue;
        if (c.show_names) {
            w.rgb(palette::ICE);
            w.str(name ? name : "stdin");
            w.reset();
            w.put(':');
        }
        if (numbered) {
            w.rgb(palette::MIST);
            w.num(n);
            w.reset();
            w.put(':');
        }
        if (!invert && w.color() && c.needle_len) {
            w.bytes(reinterpret_cast<const char*>(data) + s, at);
            w.rgb(palette::AMBER);
            w.bytes(reinterpret_cast<const char*>(data) + s + at, c.needle_len);
            w.reset();
            w.bytes(reinterpret_cast<const char*>(data) + s + at + c.needle_len, l - at - c.needle_len);
            w.put('\n');
        } else {
            put_line(w, data, s, l);
        }
    }
    if (only_count) {
        if (c.show_names) { w.str(name ? name : "stdin"); w.put(':'); }
        w.num(hits);
        w.put('\n');
    }
}

int cmd_grep(Shell& sh, Io& io, u32 argc, char** argv) {
    Options o = options_begin(argc, argv);
    NameCtx c{};
    c.sh = &sh;
    c.io = &io;
    int opt;
    while ((opt = next_option(o, "ivcn", "")) >= 0) {
        if (opt == 'i') c.flag_a = true;
        else if (opt == 'v') c.flag_b = true;
        else if (opt == 'c') c.flag_c = true;
        else if (opt == 'n') c.flag_d = true;
    }
    const char* use = "usage: grep [-i] [-v] [-c] [-n] text [file...]   (plain text, not a pattern; -i folds ASCII only)";
    if (o.bad || o.index >= argc) return usage(io, use);
    c.needle = argv[o.index++];
    c.needle_len = length(c.needle);
    c.show_names = argc - o.index > 1;
    int st = for_each_input(sh, io, o, &grep_emit, &c);
    if (sh.aborted) return 130;
    if (st != 0) return st;
    return c.matches ? 0 : 1;
}

bool is_space_cp(u32 cp) {
    return cp == ' ' || (cp >= 9 && cp <= 13) || cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
           cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

void count_emit(const char* name, const u8* data, u64 size, void* raw) {
    NameCtx& c = *static_cast<NameCtx*>(raw);
    u64 lines = 0, words = 0, cps = 0;
    bool in_word = false;
    utf8::Decoder d;
    u32 out[2];
    for (u64 i = 0; i < size; ++i) {
        if (data[i] == '\n') ++lines;
        u32 k = d.feed(data[i], out);
        for (u32 j = 0; j < k; ++j) {
            ++cps;
            bool sp = is_space_cp(out[j]);
            if (!sp && !in_word) ++words;
            in_word = !sp;
        }
    }
    u32 tail[1];
    if (d.finish(tail)) ++cps;
    if (size && data[size - 1] != '\n') ++lines;
    c.total_lines += lines;
    c.total_words += words;
    c.total_bytes += size;
    c.total_cps += cps;
    Writer& w = c.io->out;
    bool all = !c.flag_a && !c.flag_b && !c.flag_c && !c.flag_d;
    if (all || c.flag_a) { w.pad_num(lines, 8); }
    if (all || c.flag_b) { w.pad_num(words, 8); }
    if (c.flag_d) { w.pad_num(cps, 8); }
    if (all || c.flag_c) { w.pad_num(size, 8); }
    if (name) { w.put(' '); w.str(name); }
    w.put('\n');
}

int cmd_count(Shell& sh, Io& io, u32 argc, char** argv) {
    Options o = options_begin(argc, argv);
    NameCtx c{};
    c.sh = &sh;
    c.io = &io;
    int opt;
    while ((opt = next_option(o, "lwcm", "")) >= 0) {
        if (opt == 'l') c.flag_a = true;
        else if (opt == 'w') c.flag_b = true;
        else if (opt == 'c') c.flag_c = true;
        else if (opt == 'm') c.flag_d = true;
    }
    if (o.bad) return usage(io, "usage: count [-l] [-w] [-c] [-m] [file...]   (lines, words, bytes, characters)");
    int st = for_each_input(sh, io, o, &count_emit, &c);
    return sh.aborted ? 130 : st;
}

struct SortCtx {
    Shell* sh;
    Io* io;
    bool reverse, numeric, unique;
    struct Span { const u8* p; u64 len; };
    Span* spans;
    u64 count;
    u64 capacity;
    u8** owned;
    u32 owned_count;
    bool failed;
};

u64 leading_number(const u8* p, u64 len, bool* neg) {
    u64 i = 0, v = 0;
    while (i < len && (p[i] == ' ' || p[i] == '\t')) ++i;
    *neg = false;
    if (i < len && (p[i] == '-' || p[i] == '+')) { *neg = p[i] == '-'; ++i; }
    while (i < len && p[i] >= '0' && p[i] <= '9') {
        if (v < (~0ull - 9) / 10) v = v * 10 + static_cast<u64>(p[i] - '0');
        ++i;
    }
    return v;
}

int compare_spans(const SortCtx& c, const SortCtx::Span& a, const SortCtx::Span& b) {
    if (c.numeric) {
        bool na, nb;
        u64 x = leading_number(a.p, a.len, &na), y = leading_number(b.p, b.len, &nb);
        if (na != nb) return na ? -1 : 1;
        if (x != y) return (x < y) != na ? -1 : 1;
    }
    u64 n = a.len < b.len ? a.len : b.len;
    for (u64 i = 0; i < n; ++i)
        if (a.p[i] != b.p[i]) return a.p[i] < b.p[i] ? -1 : 1;
    if (a.len == b.len) return 0;
    return a.len < b.len ? -1 : 1;
}

bool sort_add(SortCtx& c, const u8* p, u64 len) {
    if (c.count == c.capacity) {
        u64 cap = c.capacity ? c.capacity * 2 : 256;
        if (cap > 1000000) return false;
        SortCtx::Span* bigger = static_cast<SortCtx::Span*>(kheap::alloc(static_cast<usize>(cap * sizeof(SortCtx::Span))));
        if (!bigger) return false;
        if (c.count) __builtin_memcpy(bigger, c.spans, static_cast<usize>(c.count * sizeof(SortCtx::Span)));
        if (c.spans) kheap::free(c.spans);
        c.spans = bigger;
        c.capacity = cap;
    }
    c.spans[c.count++] = SortCtx::Span{p, len};
    return true;
}

void sort_emit(const char*, const u8* data, u64 size, void* raw) {
    SortCtx& c = *static_cast<SortCtx*>(raw);
    if (c.failed) return;
    u8* copy = static_cast<u8*>(kheap::alloc(size ? static_cast<usize>(size) : 1));
    if (!copy) { c.failed = true; return; }
    if (size) __builtin_memcpy(copy, data, static_cast<usize>(size));
    u8** bigger = static_cast<u8**>(kheap::alloc(sizeof(u8*) * (c.owned_count + 1)));
    if (!bigger) { kheap::free(copy); c.failed = true; return; }
    if (c.owned_count) __builtin_memcpy(bigger, c.owned, sizeof(u8*) * c.owned_count);
    if (c.owned) kheap::free(c.owned);
    c.owned = bigger;
    c.owned[c.owned_count++] = copy;
    u64 pos = 0, s, l;
    while (next_line(copy, size, &pos, &s, &l))
        if (!sort_add(c, copy + s, l)) { c.failed = true; return; }
}

int cmd_sort(Shell& sh, Io& io, u32 argc, char** argv) {
    Options o = options_begin(argc, argv);
    SortCtx c{};
    c.sh = &sh;
    c.io = &io;
    int opt;
    while ((opt = next_option(o, "rnu", "")) >= 0) {
        if (opt == 'r') c.reverse = true;
        else if (opt == 'n') c.numeric = true;
        else c.unique = true;
    }
    if (o.bad) return usage(io, "usage: sort [-r] [-n] [-u] [file...]");
    int st = for_each_input(sh, io, o, &sort_emit, &c);
    if (c.failed) { print_error(io, "too many lines or out of memory"); st = 1; }
    if (!c.failed && !sh.aborted) {
        u64 n = c.count;
        u64 gap = 1;
        while (gap < n / 3) gap = gap * 3 + 1;
        for (; gap > 0; gap = (gap - 1) / 3) {
            for (u64 i = gap; i < n; ++i) {
                if (sh.poll()) break;
                SortCtx::Span tmp = c.spans[i];
                u64 j = i;
                while (j >= gap) {
                    int r = compare_spans(c, tmp, c.spans[j - gap]);
                    if (c.reverse) r = -r;
                    if (r >= 0) break;
                    c.spans[j] = c.spans[j - gap];
                    j -= gap;
                }
                c.spans[j] = tmp;
            }
            if (sh.aborted) break;
        }
        for (u64 i = 0; i < n && !sh.aborted; ++i) {
            if (c.unique && i > 0 && compare_spans(c, c.spans[i], c.spans[i - 1]) == 0) continue;
            io.out.bytes(reinterpret_cast<const char*>(c.spans[i].p), c.spans[i].len);
            io.out.put('\n');
        }
    }
    for (u32 i = 0; i < c.owned_count; ++i) kheap::free(c.owned[i]);
    if (c.owned) kheap::free(c.owned);
    if (c.spans) kheap::free(c.spans);
    return sh.aborted ? 130 : st;
}

void uniq_emit(const char*, const u8* data, u64 size, void* raw) {
    NameCtx& c = *static_cast<NameCtx*>(raw);
    Writer& w = c.io->out;
    u64 pos = 0, s, l;
    u64 prev_s = 0, prev_l = 0, run = 0;
    bool have = false;
    auto flush = [&]() {
        if (!have) return;
        if (c.flag_a) { w.pad_num(run, 6); w.put(' '); }
        put_line(w, data, prev_s, prev_l);
    };
    while (next_line(data, size, &pos, &s, &l)) {
        if (c.sh->poll()) return;
        if (have && l == prev_l && (l == 0 || __builtin_memcmp(data + s, data + prev_s, static_cast<usize>(l)) == 0)) { ++run; continue; }
        flush();
        prev_s = s;
        prev_l = l;
        run = 1;
        have = true;
    }
    flush();
}

int cmd_uniq(Shell& sh, Io& io, u32 argc, char** argv) {
    Options o = options_begin(argc, argv);
    NameCtx c{};
    c.sh = &sh;
    c.io = &io;
    int opt;
    while ((opt = next_option(o, "c", "")) >= 0) c.flag_a = true;
    if (o.bad) return usage(io, "usage: uniq [-c] [file...]   (collapses adjacent identical lines)");
    int st = for_each_input(sh, io, o, &uniq_emit, &c);
    return sh.aborted ? 130 : st;
}

struct HexCtx {
    Shell* sh;
    Io* io;
    u64 skip;
    u64 limit;
    bool limited;
};

void hex_emit(const char*, const u8* data, u64 size, void* raw) {
    HexCtx& c = *static_cast<HexCtx*>(raw);
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

int cmd_hex(Shell& sh, Io& io, u32 argc, char** argv) {
    Options o = options_begin(argc, argv);
    HexCtx c{&sh, &io, 0, 0, false};
    int opt;
    while ((opt = next_option(o, "", "sn")) >= 0) {
        if (opt == 's' && !parse_u64(o.value, &c.skip)) return usage(io, "hex: -s takes a byte offset (decimal or 0x...)");
        if (opt == 'n') {
            if (!parse_u64(o.value, &c.limit)) return usage(io, "hex: -n takes a byte count");
            c.limited = true;
        }
    }
    if (o.bad) return usage(io, "usage: hex [-s offset] [-n bytes] [file]");
    int st = for_each_input(sh, io, o, &hex_emit, &c);
    return sh.aborted ? 130 : st;
}

constexpr Command COMMANDS[] = {
    {"head", "head [-n lines] [file...]", "the first lines of files or input", &cmd_head},
    {"tail", "tail [-n lines] [file...]", "the last lines of files or input", &cmd_tail},
    {"grep", "grep [-i] [-v] [-c] [-n] text [file...]", "lines that contain text", &cmd_grep},
    {"count", "count [-l] [-w] [-c] [-m] [file...]", "lines, words, bytes, characters", &cmd_count},
    {"sort", "sort [-r] [-n] [-u] [file...]", "sort lines by bytes or by leading number", &cmd_sort},
    {"uniq", "uniq [-c] [file...]", "collapse adjacent identical lines", &cmd_uniq},
    {"hex", "hex [-s offset] [-n bytes] [file]", "hex and ASCII dump", &cmd_hex},
};

constexpr Table TABLE{"text tools", COMMANDS, sizeof(COMMANDS) / sizeof(COMMANDS[0])};

}

void register_text_commands() { register_table(&TABLE); }

}
