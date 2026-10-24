#include <cosmos/helm_rows.hpp>
#include <cosmos/kheap.hpp>

namespace helm {

namespace rows {

namespace {

constexpr u64 SET_LIMIT = 8ull << 20;
constexpr u32 MAX_COLUMNS = 12;
constexpr u32 COLUMN_NAME = 32;
constexpr u32 CELL_LIMIT = 96;

bool name_char(u8 c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
}

u32 token_name_length(const u8* l, u64 len, u64 at) {
    u64 i = at;
    while (i < len && name_char(l[i])) ++i;
    return i > at && i < len && l[i] == '=' ? static_cast<u32>(i - at) : 0;
}

bool same_bytes(const u8* a, const char* b, u32 n) {
    for (u32 i = 0; i < n; ++i)
        if (a[i] != static_cast<u8>(b[i])) return false;
    return true;
}

u64 key_length(const u8* l, u64 len) {
    for (u64 i = 0; i < len; ++i) {
        if (l[i] == ' ') return 0;
        if (l[i] == ':' && (i + 1 == len || l[i + 1] == ' ')) return i;
    }
    return 0;
}

}

u64 body_start(const u8* line, u64 len) {
    u64 k = key_length(line, len);
    if (!k) return 0;
    return k + 2 < len ? k + 2 : len;
}

bool next_token(const u8* l, u64 len, u64* pos, Token* out) {
    u64 j = *pos;
    while (j < len) {
        u32 k = (j == *pos || l[j - 1] == ' ') ? token_name_length(l, len, j) : 0;
        if (!k) { ++j; continue; }
        u64 vs = j + k + 1;
        u64 e = vs;
        while (e < len && !(l[e - 1] == ' ' && token_name_length(l, len, e))) ++e;
        u64 ve = e;
        while (ve > vs && l[ve - 1] == ' ') --ve;
        out->name_start = static_cast<u32>(j);
        out->name_len = k;
        out->value_start = static_cast<u32>(vs);
        out->value_len = static_cast<u32>(ve - vs);
        *pos = e;
        return true;
    }
    *pos = len;
    return false;
}

bool field(const u8* l, u64 len, const char* name, View* out) {
    u32 want = static_cast<u32>(cstr::length(name));
    if (want == 2 && name[0] == 'i' && name[1] == 'd') {
        u64 k = key_length(l, len);
        if (!k) return false;
        *out = View{reinterpret_cast<const char*>(l), static_cast<u32>(k)};
        return true;
    }
    u64 pos = body_start(l, len);
    Token t;
    while (next_token(l, len, &pos, &t)) {
        if (t.name_len == want && same_bytes(l + t.name_start, name, want)) {
            *out = View{reinterpret_cast<const char*>(l) + t.value_start, t.value_len};
            return true;
        }
    }
    return false;
}

bool as_number(View v, bool* negative, u64* magnitude) {
    u32 i = 0;
    *negative = false;
    if (i < v.n && (v.p[i] == '-' || v.p[i] == '+')) { *negative = v.p[i] == '-'; ++i; }
    if (i == v.n || v.n - i > 19) return false;
    u64 m = 0;
    for (; i < v.n; ++i) {
        if (v.p[i] < '0' || v.p[i] > '9') return false;
        m = m * 10 + static_cast<u64>(v.p[i] - '0');
    }
    *magnitude = m;
    if (m == 0) *negative = false;
    return true;
}

int compare(View a, View b) {
    bool na, nb;
    u64 x, y;
    if (as_number(a, &na, &x) && as_number(b, &nb, &y)) {
        if (na != nb) return na ? -1 : 1;
        if (x == y) return 0;
        return (x < y) != na ? -1 : 1;
    }
    u32 n = a.n < b.n ? a.n : b.n;
    for (u32 i = 0; i < n; ++i)
        if (a.p[i] != b.p[i]) return static_cast<u8>(a.p[i]) < static_cast<u8>(b.p[i]) ? -1 : 1;
    if (a.n == b.n) return 0;
    return a.n < b.n ? -1 : 1;
}

void add(Set& s, const u8* line, u64 len) {
    if (s.failed) return;
    if (s.size + len + 1 > s.capacity) {
        u64 cap = s.capacity ? s.capacity * 2 : 4096;
        while (cap < s.size + len + 1) cap *= 2;
        if (cap > SET_LIMIT) { s.failed = true; return; }
        u8* bigger = static_cast<u8*>(kheap::alloc(static_cast<usize>(cap)));
        if (!bigger) { s.failed = true; return; }
        if (s.size) mem::copy(bigger, s.data, static_cast<usize>(s.size));
        if (s.data) kheap::free(s.data);
        s.data = bigger;
        s.capacity = cap;
    }
    if (len) mem::copy(s.data + s.size, line, static_cast<usize>(len));
    s.size += len;
    s.data[s.size++] = '\n';
    ++s.count;
}

void release(Set& s) {
    if (s.data) kheap::free(s.data);
    s = Set{};
}

namespace {

struct Columns {
    char name[MAX_COLUMNS][COLUMN_NAME];
    u32 name_len[MAX_COLUMNS];
    u32 width[MAX_COLUMNS];
    bool numeric[MAX_COLUMNS];
    u32 count;
};

bool next_row(const Set& s, u64* pos, const u8** line, u64* len) {
    if (*pos >= s.size) return false;
    u64 i = *pos;
    while (i < s.size && s.data[i] != '\n') ++i;
    *line = s.data + *pos;
    *len = i - *pos;
    *pos = i + 1;
    return true;
}

u32 cell_width(View v) {
    u32 n = v.n < CELL_LIMIT ? v.n : CELL_LIMIT;
    return utf8::cells(v.p, n);
}

void note_column(Columns& c, const u8* name, u32 n) {
    if (n >= COLUMN_NAME) n = COLUMN_NAME - 1;
    for (u32 i = 0; i < c.count; ++i)
        if (c.name_len[i] == n && same_bytes(name, c.name[i], n)) return;
    if (c.count == MAX_COLUMNS) return;
    for (u32 i = 0; i < n; ++i) c.name[c.count][i] = static_cast<char>(name[i]);
    c.name[c.count][n] = 0;
    c.name_len[c.count] = n;
    ++c.count;
}

}

void print_table(Writer& w, const Set& s) {
    Columns c{};
    u64 pos = 0, len;
    const u8* line;
    bool any_key = false;
    while (next_row(s, &pos, &line, &len)) {
        if (key_length(line, len)) any_key = true;
        u64 tp = body_start(line, len);
        Token t;
        while (next_token(line, len, &tp, &t)) note_column(c, line + t.name_start, t.name_len);
    }
    u32 first = 0;
    Columns shown{};
    if (any_key) {
        shown.name[0][0] = 'i';
        shown.name[0][1] = 'd';
        shown.name_len[0] = 2;
        shown.count = 1;
    }
    for (u32 i = 0; i < c.count && shown.count < MAX_COLUMNS; ++i) {
        for (u32 k = 0; k <= c.name_len[i]; ++k) shown.name[shown.count][k] = c.name[i][k];
        shown.name_len[shown.count] = c.name_len[i];
        ++shown.count;
    }
    (void)first;
    if (!shown.count) {
        pos = 0;
        while (next_row(s, &pos, &line, &len)) { w.bytes(reinterpret_cast<const char*>(line), len); w.put('\n'); }
        return;
    }
    for (u32 i = 0; i < shown.count; ++i) {
        shown.width[i] = utf8::cells(shown.name[i], shown.name_len[i]);
        shown.numeric[i] = true;
    }
    pos = 0;
    while (next_row(s, &pos, &line, &len)) {
        for (u32 i = 0; i < shown.count; ++i) {
            View v;
            if (!field(line, len, shown.name[i], &v)) continue;
            u32 cw = cell_width(v);
            if (cw > shown.width[i]) shown.width[i] = cw;
            bool neg;
            u64 m;
            if (!as_number(v, &neg, &m)) shown.numeric[i] = false;
        }
    }
    w.rgb(palette::MIST);
    for (u32 i = 0; i < shown.count; ++i) {
        w.str(i ? "  " : " ");
        u32 cw = utf8::cells(shown.name[i], shown.name_len[i]);
        bool right = shown.numeric[i];
        if (right) w.spaces(shown.width[i] - cw);
        w.bytes(shown.name[i], shown.name_len[i]);
        if (!right && i + 1 < shown.count) w.spaces(shown.width[i] - cw);
    }
    w.reset();
    w.put('\n');
    pos = 0;
    while (next_row(s, &pos, &line, &len)) {
        for (u32 i = 0; i < shown.count; ++i) {
            w.str(i ? "  " : " ");
            View v{nullptr, 0};
            (void)field(line, len, shown.name[i], &v);
            u32 n = v.n < CELL_LIMIT ? v.n : CELL_LIMIT;
            u32 cw = utf8::cells(v.p, n);
            bool right = shown.numeric[i];
            if (right) w.spaces(shown.width[i] - cw);
            if (i == 0 && any_key) w.rgb(palette::ICE);
            w.bytes(v.p, n);
            if (i == 0 && any_key) w.reset();
            if (!right && i + 1 < shown.count) w.spaces(shown.width[i] - cw);
        }
        w.put('\n');
    }
    w.rgb(palette::MIST);
    w.num(s.count);
    w.str(s.count == 1 ? " row\n" : " rows\n");
    w.reset();
}

}

namespace {

using rows::View;

enum class Op { Eq, Ne, Lt, Le, Gt, Ge, Glob, Bad };

Op parse_op(const char* s) {
    struct Entry { const char* text; Op op; };
    static const Entry table[] = {{"==", Op::Eq}, {"=", Op::Eq}, {"eq", Op::Eq}, {"!=", Op::Ne}, {"ne", Op::Ne}, {"<", Op::Lt},
                                  {"lt", Op::Lt}, {"<=", Op::Le}, {"le", Op::Le}, {">", Op::Gt}, {"gt", Op::Gt}, {">=", Op::Ge},
                                  {"ge", Op::Ge}, {"~", Op::Glob}, {"match", Op::Glob}};
    for (const Entry& e : table) {
        const char* a = s;
        const char* b = e.text;
        while (*a && *a == *b) { ++a; ++b; }
        if (*a == *b) return e.op;
    }
    return Op::Bad;
}

struct WhereCtx {
    Shell* sh;
    Io* io;
    const char* field;
    Op op;
    const char* value;
    u32 value_len;
    char pattern[256];
    rows::Set kept;
    bool table;
};

bool matches(const WhereCtx& c, const u8* line, u64 len) {
    View v;
    if (!rows::field(line, len, c.field, &v)) return false;
    View want{c.value, c.value_len};
    if (c.op == Op::Glob) {
        char text[256];
        u32 n = v.n < sizeof(text) - 1 ? v.n : static_cast<u32>(sizeof(text) - 1);
        for (u32 i = 0; i < n; ++i) text[i] = v.p[i];
        text[n] = 0;
        return glob_match(c.pattern, text);
    }
    int r = rows::compare(v, want);
    switch (c.op) {
        case Op::Eq: return r == 0;
        case Op::Ne: return r != 0;
        case Op::Lt: return r < 0;
        case Op::Le: return r <= 0;
        case Op::Gt: return r > 0;
        case Op::Ge: return r >= 0;
        default: return false;
    }
}

void for_each_row(const u8* data, u64 size, Shell& sh, void* ctx, void (*fn)(void*, const u8*, u64)) {
    u64 pos = 0;
    while (pos < size) {
        if (sh.poll()) return;
        u64 i = pos;
        while (i < size && data[i] != '\n') ++i;
        if (i > pos) fn(ctx, data + pos, i - pos);
        pos = i + 1;
    }
}

void emit_row(Writer& w, const u8* line, u64 len) {
    w.bytes(reinterpret_cast<const char*>(line), len);
    w.put('\n');
}

void where_row(void* raw, const u8* line, u64 len) {
    WhereCtx& c = *static_cast<WhereCtx*>(raw);
    if (!matches(c, line, len)) return;
    if (c.table) rows::add(c.kept, line, len);
    else emit_row(c.io->out, line, len);
}

void where_input(const char*, const u8* data, u64 size, void* raw) {
    WhereCtx& c = *static_cast<WhereCtx*>(raw);
    for_each_row(data, size, *c.sh, raw, &where_row);
}

int cmd_where(Shell& sh, Io& io, u32 argc, char** argv) {
    Options o = options_begin(argc, argv);
    if (argc - o.index < 3 || parse_op(argv[o.index + 1]) == Op::Bad) {
        print_error(io, "usage: where FIELD OP VALUE [file...]   OP: == != < <= > >= ~ (or eq ne lt le gt ge match)");
        return 2;
    }
    WhereCtx c{};
    c.sh = &sh;
    c.io = &io;
    c.field = argv[o.index];
    c.op = parse_op(argv[o.index + 1]);
    c.value = argv[o.index + 2];
    c.value_len = static_cast<u32>(cstr::length(c.value));
    mark_wildcards(c.value, c.pattern, sizeof(c.pattern));
    c.table = io.out.terminal();
    o.index += 3;
    int st = for_each_input(sh, io, o, &where_input, &c);
    if (c.table && !c.kept.failed && c.kept.count) rows::print_table(io.out, c.kept);
    if (c.kept.failed) { print_error(io, "too many rows or out of memory"); st = 1; }
    rows::release(c.kept);
    return sh.aborted ? 130 : st;
}

struct PickCtx {
    Shell* sh;
    Io* io;
    char** names;
    u32 name_count;
    rows::Set kept;
    bool table;
};

void pick_row(void* raw, const u8* line, u64 len) {
    PickCtx& c = *static_cast<PickCtx*>(raw);
    u8 out[1024];
    u32 n = 0;
    auto put = [&](const char* s, u32 k) {
        for (u32 i = 0; i < k && n < sizeof(out); ++i) out[n++] = static_cast<u8>(s[i]);
    };
    View id;
    if (rows::field(line, len, "id", &id)) {
        put(id.p, id.n);
        put(":", 1);
    }
    for (u32 i = 0; i < c.name_count; ++i) {
        View v;
        if (!rows::field(line, len, c.names[i], &v)) continue;
        put(" ", 1);
        put(c.names[i], static_cast<u32>(cstr::length(c.names[i])));
        put("=", 1);
        put(v.p, v.n);
    }
    if (c.table) rows::add(c.kept, out, n);
    else emit_row(c.io->out, out, n);
}

void pick_input(const char*, const u8* data, u64 size, void* raw) {
    PickCtx& c = *static_cast<PickCtx*>(raw);
    for_each_row(data, size, *c.sh, raw, &pick_row);
}

int cmd_pick(Shell& sh, Io& io, u32 argc, char** argv) {
    Options o = options_begin(argc, argv);
    const char* file = nullptr;
    int opt;
    while ((opt = next_option(o, "", "f")) >= 0) file = o.value;
    if (o.bad || argc - o.index < 1) {
        print_error(io, "usage: pick [-f file] FIELD...   (keeps only the named fields of each row)");
        return 2;
    }
    PickCtx c{};
    c.sh = &sh;
    c.io = &io;
    c.names = argv + o.index;
    c.name_count = argc - o.index;
    c.table = io.out.terminal();
    char* source[1] = {const_cast<char*>(file)};
    Options in = options_begin(file ? 1 : 0, source);
    in.index = 0;
    int st = for_each_input(sh, io, in, &pick_input, &c);
    if (c.table && !c.kept.failed && c.kept.count) rows::print_table(io.out, c.kept);
    if (c.kept.failed) { print_error(io, "too many rows or out of memory"); st = 1; }
    rows::release(c.kept);
    return sh.aborted ? 130 : st;
}

constexpr Command COMMANDS[] = {
    {"where", "where FIELD OP VALUE [file...]", "keep the rows whose field matches; rows are 'key: field=value ...' lines", &cmd_where},
    {"pick", "pick [-f file] FIELD...", "keep only the named fields of each row", &cmd_pick},
};

constexpr Table TABLE{"rows", COMMANDS, sizeof(COMMANDS) / sizeof(COMMANDS[0])};

}

void register_row_commands() { register_table(&TABLE); }

}
