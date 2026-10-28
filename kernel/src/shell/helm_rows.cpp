#include <cosmos/helm_rows.hpp>
#include <cosmos/kheap.hpp>
#include <cosmos/civil.hpp>

namespace helm {

namespace rows {

namespace {

constexpr u32 CELL_LIMIT = 96;
constexpr u32 FORMAT_CAP = 256;

bool same(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

u32 length(const char* s) { return static_cast<u32>(cstr::length(s)); }

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

bool same_nocase(const char* a, const char* b) {
    while (*a && lower(*a) == lower(*b)) { ++a; ++b; }
    return *a == *b;
}

u64 bytes_used(const Table& t, u32 capacity, u32 pool_cap) {
    return static_cast<u64>(capacity) * t.cols * sizeof(Cell) + pool_cap;
}

bool grow_rows(Table& t) {
    u32 cap = t.capacity ? t.capacity * 2 : 64;
    if (bytes_used(t, cap, t.pool_cap) > LIMIT_BYTES) { t.failed = true; return false; }
    usize bytes = static_cast<usize>(cap) * t.cols * sizeof(Cell);
    Cell* bigger = static_cast<Cell*>(kheap::alloc(bytes));
    if (!bigger) { t.failed = true; return false; }
    usize old = static_cast<usize>(t.rows) * t.cols * sizeof(Cell);
    if (old) mem::copy(bigger, t.cells, old);
    mem::zero(reinterpret_cast<u8*>(bigger) + old, bytes - old);
    if (t.cells) kheap::free(t.cells);
    t.cells = bigger;
    t.capacity = cap;
    return true;
}

bool grow_pool(Table& t, u32 need) {
    if (t.pool_len + need <= t.pool_cap) return true;
    u64 cap = t.pool_cap ? t.pool_cap : 1024;
    while (cap < static_cast<u64>(t.pool_len) + need) cap *= 2;
    if (cap > 0x7FFFFFFFull || bytes_used(t, t.capacity, static_cast<u32>(cap)) > LIMIT_BYTES) { t.failed = true; return false; }
    char* bigger = static_cast<char*>(kheap::alloc(static_cast<usize>(cap)));
    if (!bigger) { t.failed = true; return false; }
    if (t.pool_len) mem::copy(bigger, t.pool, t.pool_len);
    if (t.pool) kheap::free(t.pool);
    t.pool = bigger;
    t.pool_cap = static_cast<u32>(cap);
    return true;
}

Cell& cell(Table& t, u32 row, u32 col) { return t.cells[static_cast<usize>(row) * t.cols + col]; }
const Cell& cell(const Table& t, u32 row, u32 col) { return t.cells[static_cast<usize>(row) * t.cols + col]; }

bool valid(const Table& t, u32 row, u32 col) { return row < t.rows && col < t.cols; }

u32 clip(const char* s, u32 n, u32 limit) {
    if (n <= limit) return n;
    u32 k = limit;
    while (k > 0 && (static_cast<u8>(s[k]) & 0xC0) == 0x80) --k;
    return k;
}

u32 put_num(char* out, u32 cap, u64 v) {
    char tmp[24];
    u32 i = 0;
    if (!v) tmp[i++] = '0';
    while (v) { tmp[i++] = static_cast<char>('0' + v % 10); v /= 10; }
    u32 n = 0;
    while (i && n + 1 < cap) out[n++] = tmp[--i];
    return n;
}

u32 put_str(char* out, u32 cap, u32 at, const char* s) {
    while (*s && at + 1 < cap) out[at++] = *s++;
    return at;
}

void cell_safe(Writer& w, const char* s, u32 n) {
    utf8::Decoder d;
    u32 cps[2];
    for (u32 i = 0; i < n; ++i) {
        u32 k = d.feed(static_cast<u8>(s[i]), cps);
        for (u32 j = 0; j < k; ++j) {
            u32 cp = cps[j];
            if (cp < 0x20) w.cp(0x2400 + cp);
            else if (cp == 0x7F) w.cp(0x2421);
            else if (cp >= 0x80 && cp < 0xA0) w.cp(utf8::REPLACEMENT);
            else w.cp(cp);
        }
    }
    u32 tail[1];
    if (d.finish(tail)) w.cp(utf8::REPLACEMENT);
}

bool numeric_kind(Kind k) { return k == Kind::Int || k == Kind::Size; }

i32 find_kind_column(const Table& t) { return find_column(t, "kind"); }

bool row_is_dir(const Table& t, i32 kind_col, u32 row) {
    if (kind_col < 0) return false;
    u32 n = 0;
    const char* k = text_at(t, row, static_cast<u32>(kind_col), &n);
    return n == 3 && k[0] == 'd' && k[1] == 'i' && k[2] == 'r';
}

int compare_text(const char* a, u32 an, const char* b, u32 bn) {
    u32 n = an < bn ? an : bn;
    for (u32 i = 0; i < n; ++i)
        if (a[i] != b[i]) return static_cast<u8>(a[i]) < static_cast<u8>(b[i]) ? -1 : 1;
    if (an == bn) return 0;
    return an < bn ? -1 : 1;
}

u64 leading_number(const char* p, u32 len, bool* neg) {
    u32 i = 0;
    u64 v = 0;
    while (i < len && (p[i] == ' ' || p[i] == '\t')) ++i;
    *neg = false;
    if (i < len && (p[i] == '-' || p[i] == '+')) { *neg = p[i] == '-'; ++i; }
    while (i < len && p[i] >= '0' && p[i] <= '9') {
        if (v < (~0ull - 9) / 10) v = v * 10 + static_cast<u64>(p[i] - '0');
        ++i;
    }
    return v;
}

int row_compare(const Table& t, u32 a, u32 b, const u32* cols, u32 nc, bool lead, bool desc) {
    for (u32 i = 0; i < nc; ++i) {
        int r = compare_cells(t, a, cols[i], t, b, cols[i], lead);
        if (!r) continue;
        bool nulls = is_null(t, a, cols[i]) || is_null(t, b, cols[i]);
        return desc && !nulls ? -r : r;
    }
    return 0;
}

bool sort_indices(const Table& t, u32* idx, u32 n, const u32* cols, u32 nc, bool desc, bool lead) {
    if (n < 2) return true;
    u32* tmp = static_cast<u32*>(kheap::alloc(sizeof(u32) * n));
    if (!tmp) return false;
    u32* src = idx;
    u32* dst = tmp;
    for (u32 width = 1; width < n; width *= 2) {
        for (u32 lo = 0; lo < n; lo += 2 * width) {
            u32 mid = lo + width < n ? lo + width : n;
            u32 hi = lo + 2 * width < n ? lo + 2 * width : n;
            u32 i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                int r = row_compare(t, src[i], src[j], cols, nc, lead, desc);
                if (r <= 0) dst[k++] = src[i++];
                else dst[k++] = src[j++];
            }
            while (i < mid) dst[k++] = src[i++];
            while (j < hi) dst[k++] = src[j++];
        }
        u32* swap = src;
        src = dst;
        dst = swap;
    }
    if (src != idx) mem::copy(idx, src, sizeof(u32) * n);
    kheap::free(tmp);
    return true;
}

}

void begin(Table& t) {
    t = Table{};
    t.body = -1;
}

void release(Table& t) {
    if (t.cells) kheap::free(t.cells);
    if (t.pool) kheap::free(t.pool);
    t = Table{};
    t.body = -1;
}

bool add_column(Table& t, const char* name, Kind kind) {
    u32 n = length(name);
    if (t.rows != 0 || t.cols >= MAX_COLUMNS || n == 0 || n >= NAME_BYTES) { t.failed = true; return false; }
    Column& c = t.col[t.cols++];
    mem::copy(c.name, name, n + 1);
    c.kind = kind;
    return true;
}

u32 add_row(Table& t) {
    if (t.failed || t.cols == 0) return ~0u;
    if (t.rows == t.capacity && !grow_rows(t)) return ~0u;
    return t.rows++;
}

void set_text(Table& t, u32 row, u32 col, const char* s, u32 n) {
    if (!valid(t, row, col) || t.failed) return;
    if (!grow_pool(t, n + 1)) return;
    Cell& c = cell(t, row, col);
    c.off = t.pool_len;
    c.len = n;
    if (n) mem::copy(t.pool + t.pool_len, s, n);
    t.pool[t.pool_len + n] = 0;
    t.pool_len += n + 1;
}

void set_text(Table& t, u32 row, u32 col, const char* s) { set_text(t, row, col, s, length(s)); }

void set_num(Table& t, u32 row, u32 col, u64 v) {
    if (!valid(t, row, col)) return;
    Cell& c = cell(t, row, col);
    c.num = v;
    c.len = 0;
}

void set_null(Table& t, u32 row, u32 col) {
    if (!valid(t, row, col)) return;
    Cell& c = cell(t, row, col);
    c.num = 0;
    c.len = NULL_LEN;
}

void set_note(Table& t, const char* s) {
    u32 n = length(s);
    if (n >= NOTE_BYTES) n = NOTE_BYTES - 1;
    mem::copy(t.note, s, n);
    t.note[n] = 0;
}

i32 find_column(const Table& t, const char* name) {
    for (u32 i = 0; i < t.cols; ++i)
        if (same(t.col[i].name, name)) return static_cast<i32>(i);
    return -1;
}

bool is_null(const Table& t, u32 row, u32 col) {
    return valid(t, row, col) && t.col[col].kind != Kind::Text && cell(t, row, col).len == NULL_LEN;
}

const char* text_at(const Table& t, u32 row, u32 col, u32* n) {
    if (!valid(t, row, col) || t.col[col].kind != Kind::Text) { *n = 0; return ""; }
    const Cell& c = cell(t, row, col);
    *n = c.len;
    return t.pool + c.off;
}

u64 num_at(const Table& t, u32 row, u32 col) {
    return valid(t, row, col) ? cell(t, row, col).num : 0;
}

int compare_cells(const Table& a, u32 row_a, u32 col_a, const Table& b, u32 row_b, u32 col_b, bool leading) {
    Kind ka = a.col[col_a].kind;
    if (ka == Kind::Text) {
        u32 an, bn;
        const char* pa = text_at(a, row_a, col_a, &an);
        const char* pb = text_at(b, row_b, col_b, &bn);
        if (leading) {
            bool na, nb;
            u64 x = leading_number(pa, an, &na), y = leading_number(pb, bn, &nb);
            if (na != nb) return na ? -1 : 1;
            if (x != y) return (x < y) != na ? -1 : 1;
        }
        return compare_text(pa, an, pb, bn);
    }
    bool nulla = is_null(a, row_a, col_a), nullb = is_null(b, row_b, col_b);
    if (nulla || nullb) return nulla == nullb ? 0 : (nulla ? 1 : -1);
    u64 x = num_at(a, row_a, col_a), y = num_at(b, row_b, col_b);
    if (ka == Kind::Int) {
        i64 sx = static_cast<i64>(x), sy = static_cast<i64>(y);
        return sx == sy ? 0 : (sx < sy ? -1 : 1);
    }
    return x == y ? 0 : (x < y ? -1 : 1);
}

bool from_text(Table& t, const u8* data, u64 size) {
    begin(t);
    if (!add_column(t, "text", Kind::Text)) return false;
    t.body = 0;
    u64 pos = 0;
    while (pos < size) {
        u64 i = pos;
        while (i < size && data[i] != '\n') ++i;
        u32 r = add_row(t);
        if (r == ~0u) return false;
        set_text(t, r, 0, reinterpret_cast<const char*>(data) + pos, static_cast<u32>(i - pos));
        if (t.failed) return false;
        pos = i + 1;
    }
    return true;
}

bool copy_schema(Table& dst, const Table& src) {
    begin(dst);
    for (u32 i = 0; i < src.cols; ++i)
        if (!add_column(dst, src.col[i].name, src.col[i].kind)) return false;
    dst.body = src.body;
    return true;
}

bool copy_row(Table& dst, const Table& src, u32 row) {
    u32 r = add_row(dst);
    if (r == ~0u) return false;
    for (u32 c = 0; c < src.cols && c < dst.cols; ++c) {
        if (src.col[c].kind == Kind::Text) {
            u32 n;
            const char* s = text_at(src, row, c, &n);
            set_text(dst, r, c, s, n);
        } else {
            cell(dst, r, c) = cell(src, row, c);
        }
    }
    return !dst.failed;
}

u32 format_cell(const Table& t, u32 row, u32 col, char* out, u32 cap) {
    if (cap == 0) return 0;
    out[0] = 0;
    if (!valid(t, row, col)) return 0;
    Kind kind = t.col[col].kind;
    if (kind == Kind::Text) {
        u32 n;
        const char* s = text_at(t, row, col, &n);
        if (n + 1 > cap) n = clip(s, cap - 1, cap - 1);
        mem::copy(out, s, n);
        out[n] = 0;
        return n;
    }
    if (is_null(t, row, col)) { out[0] = '-'; out[1] = 0; return 1; }
    u64 v = num_at(t, row, col);
    u32 n = 0;
    switch (kind) {
        case Kind::Int: {
            i64 s = static_cast<i64>(v);
            if (s < 0) { out[n++] = '-'; v = 0 - v; }
            n += put_num(out + n, cap - n, v);
            break;
        }
        case Kind::Size: {
            if (v < 1024) {
                n = put_num(out, cap, v);
                n = put_str(out, cap, n, " B");
                break;
            }
            static const char* units[] = {"KiB", "MiB", "GiB", "TiB"};
            u32 u = 0;
            u64 whole = v, rem = 0;
            while (whole >= 1024 && u < 3) { rem = whole % 1024; whole /= 1024; ++u; }
            n = put_num(out, cap, whole);
            if (n + 1 < cap) out[n++] = '.';
            n += put_num(out + n, cap - n, rem * 10 / 1024);
            if (n + 1 < cap) out[n++] = ' ';
            n = put_str(out, cap, n, units[u - 1]);
            break;
        }
        case Kind::Time: {
            char stamp[24];
            format_time(v, stamp);
            n = put_str(out, cap, 0, stamp);
            break;
        }
        case Kind::Flag:
            n = put_str(out, cap, 0, v ? "yes" : "no");
            break;
        default:
            break;
    }
    out[n] = 0;
    return n;
}

void render(Writer& w, const Table& t) {
    if (t.cols == 0) return;
    bool plain = t.cols == 1 && t.body == 0 && t.col[0].kind == Kind::Text;
    if (plain) {
        for (u32 r = 0; r < t.rows; ++r) {
            u32 n;
            const char* s = text_at(t, r, 0, &n);
            write_safe(w, reinterpret_cast<const u8*>(s), n);
            w.put('\n');
        }
        if (t.note[0]) { w.rgb(palette::MIST); w.str(t.note); w.reset(); w.put('\n'); }
        return;
    }
    u32 width[MAX_COLUMNS];
    i32 kind_col = find_kind_column(t);
    i32 name_col = find_column(t, "name");
    char buf[FORMAT_CAP];
    for (u32 c = 0; c < t.cols; ++c) width[c] = utf8::cells(t.col[c].name, length(t.col[c].name));
    for (u32 r = 0; r < t.rows; ++r) {
        for (u32 c = 0; c < t.cols; ++c) {
            u32 n = format_cell(t, r, c, buf, CELL_LIMIT + 1);
            u32 cw = utf8::cells(buf, n);
            if (static_cast<i32>(c) == name_col && row_is_dir(t, kind_col, r)) ++cw;
            if (cw > width[c]) width[c] = cw;
        }
    }
    w.rgb(palette::MIST);
    if (t.rows) {
        for (u32 c = 0; c < t.cols; ++c) {
            w.str(c ? "  " : " ");
            u32 cw = utf8::cells(t.col[c].name, length(t.col[c].name));
            bool right = numeric_kind(t.col[c].kind);
            if (right) w.spaces(width[c] - cw);
            w.str(t.col[c].name);
            if (!right && c + 1 < t.cols) w.spaces(width[c] - cw);
        }
        w.reset();
        w.put('\n');
    }
    for (u32 r = 0; r < t.rows; ++r) {
        for (u32 c = 0; c < t.cols; ++c) {
            w.str(c ? "  " : " ");
            u32 n = format_cell(t, r, c, buf, CELL_LIMIT + 1);
            u32 cw = utf8::cells(buf, n);
            bool dir_name = static_cast<i32>(c) == name_col && row_is_dir(t, kind_col, r);
            if (dir_name) ++cw;
            bool right = numeric_kind(t.col[c].kind);
            bool dim = t.col[c].kind != Kind::Text && is_null(t, r, c);
            if (right) w.spaces(width[c] - cw);
            if (dir_name) w.rgb(palette::ICE);
            else if (dim) w.rgb(palette::MIST);
            cell_safe(w, buf, n);
            if (dir_name) w.put('/');
            if (dir_name || dim) w.reset();
            if (!right && c + 1 < t.cols) w.spaces(width[c] - cw);
        }
        w.put('\n');
    }
    w.rgb(palette::MIST);
    w.num(t.rows);
    w.str(t.rows == 1 ? " row" : " rows");
    if (t.note[0]) { w.str("  "); w.str(t.note); }
    w.put('\n');
    w.reset();
}

void flatten(Writer& w, const Table& t) {
    if (t.cols == 0) return;
    char buf[FORMAT_CAP];
    if (t.body >= 0 && static_cast<u32>(t.body) < t.cols) {
        u32 c = static_cast<u32>(t.body);
        for (u32 r = 0; r < t.rows; ++r) {
            if (t.col[c].kind == Kind::Text) {
                u32 n;
                const char* s = text_at(t, r, c, &n);
                w.bytes(s, n);
            } else {
                format_cell(t, r, c, buf, sizeof(buf));
                w.str(buf);
            }
            w.put('\n');
        }
        return;
    }
    u32 width[MAX_COLUMNS];
    for (u32 c = 0; c < t.cols; ++c) width[c] = utf8::cells(t.col[c].name, length(t.col[c].name));
    for (u32 r = 0; r < t.rows; ++r)
        for (u32 c = 0; c < t.cols; ++c) {
            u32 n = format_cell(t, r, c, buf, sizeof(buf));
            u32 cw = utf8::cells(buf, n);
            if (cw > width[c]) width[c] = cw;
        }
    for (u32 c = 0; c < t.cols; ++c) {
        if (c) w.str("  ");
        w.str(t.col[c].name);
        if (c + 1 < t.cols) w.spaces(width[c] - utf8::cells(t.col[c].name, length(t.col[c].name)));
    }
    w.put('\n');
    for (u32 r = 0; r < t.rows; ++r) {
        for (u32 c = 0; c < t.cols; ++c) {
            if (c) w.str("  ");
            u32 n = format_cell(t, r, c, buf, sizeof(buf));
            bool right = numeric_kind(t.col[c].kind);
            u32 cw = utf8::cells(buf, n);
            if (right) w.spaces(width[c] - cw);
            w.bytes(buf, n);
            if (!right && c + 1 < t.cols) w.spaces(width[c] - cw);
        }
        w.put('\n');
    }
}

bool parse_value(Kind kind, const char* s, u64* out) {
    if (kind == Kind::Int) {
        bool neg = false;
        if (*s == '-' || *s == '+') { neg = *s == '-'; ++s; }
        if (!*s) return false;
        u64 v = 0;
        for (; *s; ++s) {
            if (*s < '0' || *s > '9') return false;
            u64 d = static_cast<u64>(*s - '0');
            if (v > (0x7FFFFFFFFFFFFFFFull - d) / 10) return false;
            v = v * 10 + d;
        }
        *out = neg ? 0 - v : v;
        return true;
    }
    if (kind == Kind::Size) {
        if (*s < '0' || *s > '9') return false;
        u64 v = 0;
        while (*s >= '0' && *s <= '9') {
            u64 d = static_cast<u64>(*s - '0');
            if (v > (~0ull - d) / 10) return false;
            v = v * 10 + d;
            ++s;
        }
        u32 shift = 0;
        if (*s) {
            static const char* names[][3] = {{"b", "", ""}, {"k", "kb", "kib"}, {"m", "mb", "mib"}, {"g", "gb", "gib"}, {"t", "tb", "tib"}};
            bool found = false;
            for (u32 i = 0; i < 5 && !found; ++i)
                for (u32 k = 0; k < 3; ++k)
                    if (names[i][k][0] && same_nocase(s, names[i][k])) { shift = i * 10; found = true; break; }
            if (!found) return false;
        }
        if (shift && v > (~0ull >> shift)) return false;
        *out = v << shift;
        return true;
    }
    if (kind == Kind::Flag) {
        static const char* yes[] = {"yes", "true", "on", "1"};
        static const char* no[] = {"no", "false", "off", "0"};
        for (const char* y : yes) if (same_nocase(s, y)) { *out = 1; return true; }
        for (const char* n : no) if (same_nocase(s, n)) { *out = 0; return true; }
        return false;
    }
    if (kind == Kind::Time) {
        u32 field[6] = {0, 0, 0, 0, 0, 0};
        u32 digits[6] = {4, 2, 2, 2, 2, 2};
        const char sep[6] = {0, '-', '-', ' ', ':', ':'};
        u32 parsed = 0;
        const char* p = s;
        for (u32 i = 0; i < 6; ++i) {
            if (i > 0) {
                if (!*p) break;
                char c = *p;
                if (i == 3 && c == 'T') c = ' ';
                if (c != sep[i]) return false;
                ++p;
            }
            for (u32 d = 0; d < digits[i]; ++d) {
                if (*p < '0' || *p > '9') return false;
                field[i] = field[i] * 10 + static_cast<u32>(*p - '0');
                ++p;
            }
            ++parsed;
        }
        if (*p || parsed < 3 || parsed == 4) return false;
        civil::Date d{field[0], field[1], field[2], field[3], field[4], field[5]};
        if (field[0] < 1970 || !civil::valid(d)) return false;
        *out = civil::to_unix(d) * 1000ull;
        return true;
    }
    return false;
}

bool sort_rows(Table& out, const Table& in, const u32* columns, u32 column_count, bool descending, bool leading_number) {
    u32* idx = static_cast<u32*>(kheap::alloc(sizeof(u32) * (in.rows ? in.rows : 1)));
    if (!idx) return false;
    for (u32 i = 0; i < in.rows; ++i) idx[i] = i;
    bool ok = sort_indices(in, idx, in.rows, columns, column_count, descending, leading_number);
    if (ok) ok = copy_schema(out, in);
    for (u32 i = 0; ok && i < in.rows; ++i) ok = copy_row(out, in, idx[i]);
    kheap::free(idx);
    return ok;
}

}

namespace {

using namespace rows;

using Rows = rows::Table;

enum class Op { Eq, Ne, Lt, Le, Gt, Ge, Glob, Bad };

Op parse_op(const char* s) {
    struct Entry { const char* text; Op op; };
    static const Entry table[] = {{"==", Op::Eq}, {"=", Op::Eq}, {"eq", Op::Eq}, {"!=", Op::Ne}, {"ne", Op::Ne}, {"<", Op::Lt},
                                  {"lt", Op::Lt}, {"<=", Op::Le}, {"le", Op::Le}, {">", Op::Gt}, {"gt", Op::Gt}, {">=", Op::Ge},
                                  {"ge", Op::Ge}, {"~", Op::Glob}, {"match", Op::Glob}};
    for (const Entry& e : table)
        if (same(s, e.text)) return e.op;
    return Op::Bad;
}

int usage(Io& io, const char* text) {
    print_error(io, text);
    return 2;
}

bool need_rows(Io& io) {
    if (io.in_rows) return true;
    print_error(io, "no rows: pipe something in, for example  chart | where size > 1KiB");
    return false;
}

void list_columns(const Rows& t, char* out, u32 cap) {
    u32 n = 0;
    for (u32 i = 0; i < t.cols; ++i) {
        if (i && n + 1 < cap) out[n++] = ' ';
        for (const char* p = t.col[i].name; *p && n + 1 < cap; ++p) out[n++] = *p;
    }
    out[n] = 0;
}

bool column_or_error(Io& io, const Rows& t, const char* name, u32* col) {
    i32 c = find_column(t, name);
    if (c >= 0) { *col = static_cast<u32>(c); return true; }
    char known[160];
    list_columns(t, known, sizeof(known));
    io.err.rgb(palette::SAND);
    io.err.str("  no column '");
    io.err.str(name);
    io.err.str("' (columns: ");
    io.err.str(known);
    io.err.str(")\n");
    io.err.reset();
    return false;
}

bool fail_table(Io& io, Rows& out) {
    if (!out.failed) return false;
    print_error(io, "too many rows or out of memory");
    return true;
}

struct Where {
    u32 col;
    Op op;
    u64 num;
    const char* text;
    u32 text_len;
    char pattern[256];
};

bool test_row(const Rows& t, u32 row, const Where& w) {
    if (t.col[w.col].kind == Kind::Text) {
        u32 n;
        const char* s = text_at(t, row, w.col, &n);
        if (w.op == Op::Glob) {
            char hay[256];
            u32 k = n < sizeof(hay) - 1 ? n : static_cast<u32>(sizeof(hay) - 1);
            mem::copy(hay, s, k);
            hay[k] = 0;
            return glob_match(w.pattern, hay);
        }
        int r = compare_text(s, n, w.text, w.text_len);
        switch (w.op) {
            case Op::Eq: return r == 0;
            case Op::Ne: return r != 0;
            case Op::Lt: return r < 0;
            case Op::Le: return r <= 0;
            case Op::Gt: return r > 0;
            case Op::Ge: return r >= 0;
            default: return false;
        }
    }
    if (is_null(t, row, w.col)) return false;
    u64 v = num_at(t, row, w.col);
    int r;
    if (t.col[w.col].kind == Kind::Int) {
        i64 a = static_cast<i64>(v), b = static_cast<i64>(w.num);
        r = a == b ? 0 : (a < b ? -1 : 1);
    } else {
        r = v == w.num ? 0 : (v < w.num ? -1 : 1);
    }
    switch (w.op) {
        case Op::Eq: return r == 0;
        case Op::Ne: return r != 0;
        case Op::Lt: return r < 0;
        case Op::Le: return r <= 0;
        case Op::Gt: return r > 0;
        case Op::Ge: return r >= 0;
        default: return false;
    }
}

int cmd_where(Shell& sh, Io& io, u32 argc, char** argv) {
    if (argc != 4 || parse_op(argv[2]) == Op::Bad)
        return usage(io, "usage: where COLUMN OP VALUE   OP: = != < <= > >= ~ (or eq ne lt le gt ge match)");
    if (!need_rows(io)) return 1;
    const Rows& in = *io.in_rows;
    Where w{};
    if (!column_or_error(io, in, argv[1], &w.col)) return 1;
    w.op = parse_op(argv[2]);
    w.text = argv[3];
    w.text_len = length(argv[3]);
    Kind kind = in.col[w.col].kind;
    if (kind == Kind::Text) {
        mark_wildcards(argv[3], w.pattern, sizeof(w.pattern));
    } else {
        if (w.op == Op::Glob) return usage(io, "where: ~ matches text columns only");
        if (!parse_value(kind, argv[3], &w.num)) {
            io.err.rgb(palette::SAND);
            io.err.str("  '");
            io.err.str(argv[3]);
            io.err.str("' is not a valid ");
            io.err.str(kind == Kind::Int ? "number" : kind == Kind::Size ? "size (for example 4096 or 2KiB)" : kind == Kind::Time ? "time (YYYY-MM-DD or YYYY-MM-DD HH:MM)" : "yes or no");
            io.err.str(" for column ");
            io.err.str(argv[1]);
            io.err.str("\n");
            io.err.reset();
            return 2;
        }
    }
    Rows& out = *io.out_rows;
    if (!copy_schema(out, in)) return fail_table(io, out) ? 1 : 1;
    for (u32 r = 0; r < in.rows; ++r) {
        if (sh.poll()) return 130;
        if (test_row(in, r, w) && !copy_row(out, in, r)) break;
    }
    return fail_table(io, out) ? 1 : 0;
}

void copy_cell(Rows& dst, u32 dr, u32 dc, const Rows& src, u32 sr, u32 sc) {
    if (src.col[sc].kind == Kind::Text) {
        u32 n;
        const char* s = text_at(src, sr, sc, &n);
        set_text(dst, dr, dc, s, n);
    } else if (is_null(src, sr, sc)) {
        set_null(dst, dr, dc);
    } else {
        set_num(dst, dr, dc, num_at(src, sr, sc));
    }
}

int cmd_pick(Shell& sh, Io& io, u32 argc, char** argv) {
    if (argc < 2) return usage(io, "usage: pick COLUMN...   (keeps only the named columns, in the order given)");
    if (!need_rows(io)) return 1;
    const Rows& in = *io.in_rows;
    u32 map[MAX_COLUMNS];
    u32 n = argc - 1;
    if (n > MAX_COLUMNS) return usage(io, "pick: too many columns");
    for (u32 i = 0; i < n; ++i)
        if (!column_or_error(io, in, argv[1 + i], &map[i])) return 1;
    Rows& out = *io.out_rows;
    begin(out);
    for (u32 i = 0; i < n; ++i)
        if (!add_column(out, in.col[map[i]].name, in.col[map[i]].kind)) return fail_table(io, out) ? 1 : 1;
    for (u32 i = 0; i < n; ++i)
        if (in.body >= 0 && map[i] == static_cast<u32>(in.body)) out.body = static_cast<i32>(i);
    if (out.body < 0 && n == 1 && in.col[map[0]].kind == Kind::Text) out.body = 0;
    for (u32 r = 0; r < in.rows; ++r) {
        if (sh.poll()) return 130;
        u32 dr = add_row(out);
        if (dr == ~0u) break;
        for (u32 i = 0; i < n; ++i) copy_cell(out, dr, i, in, r, map[i]);
    }
    return fail_table(io, out) ? 1 : 0;
}

int cmd_order(Shell& sh, Io& io, u32 argc, char** argv) {
    if (!need_rows(io)) return 1;
    const Rows& in = *io.in_rows;
    bool down = false, lead = false;
    Flag f = opt_flag(io, "down");
    if (f == Flag::Bad) return usage(io, "order: down takes yes or no");
    down = f == Flag::Yes;
    f = opt_flag(io, "number");
    if (f == Flag::Bad) return usage(io, "order: number takes yes or no");
    lead = f == Flag::Yes;
    u32 cols[MAX_COLUMNS];
    u32 nc = 0;
    if (argc == 1) {
        cols[nc++] = in.body >= 0 ? static_cast<u32>(in.body) : 0;
    } else {
        if (argc - 1 > MAX_COLUMNS) return usage(io, "order: too many columns");
        for (u32 i = 1; i < argc; ++i)
            if (!column_or_error(io, in, argv[i], &cols[nc++])) return 1;
    }
    if (in.rows > 1000000) { print_error(io, "too many rows"); return 1; }
    Rows& out = *io.out_rows;
    if (!sort_rows(out, in, cols, nc, down, lead)) { print_error(io, "too many rows or out of memory"); return 1; }
    (void)sh;
    return 0;
}

int slice(Shell& sh, Io& io, u32 argc, char** argv, bool from_end, const char* use) {
    u64 want = 10;
    if (argc > 2 || (argc == 2 && !parse_u64(argv[1], &want))) return usage(io, use);
    if (!need_rows(io)) return 1;
    const Rows& in = *io.in_rows;
    Rows& out = *io.out_rows;
    if (!copy_schema(out, in)) return fail_table(io, out) ? 1 : 1;
    u64 first = 0, last = in.rows;
    if (from_end) first = in.rows > want ? in.rows - want : 0;
    else last = want < in.rows ? want : in.rows;
    for (u64 r = first; r < last; ++r) {
        if (sh.poll()) return 130;
        if (!copy_row(out, in, static_cast<u32>(r))) break;
    }
    return fail_table(io, out) ? 1 : 0;
}

int cmd_first(Shell& sh, Io& io, u32 argc, char** argv) { return slice(sh, io, argc, argv, false, "usage: first [count]   (default 10)"); }
int cmd_last(Shell& sh, Io& io, u32 argc, char** argv) { return slice(sh, io, argc, argv, true, "usage: last [count]   (default 10)"); }

int cmd_distinct(Shell& sh, Io& io, u32 argc, char** argv) {
    if (argc > 2) return usage(io, "usage: distinct [COLUMN]   (keeps the first row for each value; all columns when none is named)");
    if (!need_rows(io)) return 1;
    const Rows& in = *io.in_rows;
    u32 cols[MAX_COLUMNS];
    u32 nc = 0;
    if (argc == 2) {
        if (!column_or_error(io, in, argv[1], &cols[nc++])) return 1;
    } else {
        for (u32 i = 0; i < in.cols; ++i) cols[nc++] = i;
    }
    u32 n = in.rows;
    u32* idx = static_cast<u32*>(kheap::alloc(sizeof(u32) * (n ? n : 1)));
    bool* keep = static_cast<bool*>(kheap::alloc(n ? n : 1));
    if (!idx || !keep) {
        if (idx) kheap::free(idx);
        if (keep) kheap::free(keep);
        print_error(io, "out of memory");
        return 1;
    }
    for (u32 i = 0; i < n; ++i) { idx[i] = i; keep[i] = false; }
    bool ok = sort_indices(in, idx, n, cols, nc, false, false);
    if (ok) {
        for (u32 i = 0; i < n; ++i) {
            bool first = i == 0 || row_compare(in, idx[i], idx[i - 1], cols, nc, false, false) != 0;
            if (first) keep[idx[i]] = true;
        }
    }
    Rows& out = *io.out_rows;
    if (ok) ok = copy_schema(out, in);
    for (u32 r = 0; ok && r < n; ++r) {
        if (sh.poll()) { ok = false; break; }
        if (keep[r]) ok = copy_row(out, in, r);
    }
    kheap::free(idx);
    kheap::free(keep);
    if (sh.aborted) return 130;
    if (!ok) { print_error(io, "too many rows or out of memory"); return 1; }
    return 0;
}

bool is_space_cp(u32 cp) {
    return cp == ' ' || (cp >= 9 && cp <= 13) || cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
           cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

struct TallyCtx {
    Shell* sh;
    Rows* out;
    bool named;
    bool failed;
};

void tally_bytes(const char* name, const u8* data, u64 size, void* raw) {
    TallyCtx& c = *static_cast<TallyCtx*>(raw);
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
    u32 r = add_row(*c.out);
    if (r == ~0u) { c.failed = true; return; }
    u32 col = 0;
    if (c.named) set_text(*c.out, r, col++, name ? name : "input");
    set_num(*c.out, r, col++, lines);
    set_num(*c.out, r, col++, words);
    set_num(*c.out, r, col++, cps);
    set_num(*c.out, r, col++, size);
}

int tally_groups(Io& io, const Rows& in, const char* by, Rows& out) {
    u32 col;
    if (!column_or_error(io, in, by, &col)) return 1;
    u32 n = in.rows;
    u32* idx = static_cast<u32*>(kheap::alloc(sizeof(u32) * (n ? n : 1)));
    if (!idx) { print_error(io, "out of memory"); return 1; }
    for (u32 i = 0; i < n; ++i) idx[i] = i;
    bool ok = sort_indices(in, idx, n, &col, 1, false, false);
    if (ok) {
        begin(out);
        add_column(out, in.col[col].name, in.col[col].kind);
        add_column(out, "rows", Kind::Int);
        u32 i = 0;
        while (i < n) {
            u32 j = i + 1;
            while (j < n && compare_cells(in, idx[j], col, in, idx[i], col, false) == 0) ++j;
            u32 r = add_row(out);
            if (r == ~0u) break;
            copy_cell(out, r, 0, in, idx[i], col);
            set_num(out, r, 1, j - i);
            i = j;
        }
    }
    kheap::free(idx);
    if (!ok || out.failed) { print_error(io, "too many rows or out of memory"); return 1; }
    return 0;
}

int cmd_tally(Shell& sh, Io& io, u32 argc, char** argv) {
    Rows& out = *io.out_rows;
    begin(out);
    const char* by = opt(io, "by");
    const Rows* source = io.in_rows;
    Rows lines;
    begin(lines);
    if (!source && by && io.has_in) {
        if (!from_text(lines, io.in, io.in_size)) {
            release(lines);
            print_error(io, "too many lines or out of memory");
            return 1;
        }
        source = &lines;
    }
    if (source) {
        int st = 0;
        if (argc != 1) st = usage(io, "usage: tally   (counts the rows piped in; by: COLUMN counts each value)");
        else if (!by) {
            add_column(out, "rows", Kind::Int);
            u32 r = add_row(out);
            if (r == ~0u) st = fail_table(io, out) ? 1 : 1;
            else set_num(out, r, 0, source->rows);
        } else {
            st = tally_groups(io, *source, by, out);
        }
        release(lines);
        return st;
    }
    if (by) return usage(io, "tally: by: needs rows or text piped in");
    TallyCtx c{&sh, &out, argc > 1, false};
    if (c.named) add_column(out, "file", Kind::Text);
    add_column(out, "lines", Kind::Int);
    add_column(out, "words", Kind::Int);
    add_column(out, "chars", Kind::Int);
    add_column(out, "bytes", Kind::Int);
    int st = for_each_input(sh, io, argc, argv, 1, &tally_bytes, &c);
    if (c.failed || out.failed) { print_error(io, "too many rows or out of memory"); return 1; }
    return sh.aborted ? 130 : st;
}

constexpr Command COMMANDS[] = {
    {"where", "where COLUMN OP VALUE", "keep the rows whose column matches; sizes (2KiB) and times (2026-10-01) compare as such", &cmd_where, "", CMD_ROWS | CMD_LITERAL},
    {"pick", "pick COLUMN...", "keep only the named columns", &cmd_pick, "", CMD_ROWS},
    {"order", "order [COLUMN...]  down: yes  number: yes", "sort rows by one or more columns; number: yes sorts text by its leading number", &cmd_order, "down number", CMD_ROWS},
    {"first", "first [count]", "the first rows, ten by default", &cmd_first, "", CMD_ROWS},
    {"last", "last [count]", "the last rows, ten by default", &cmd_last, "", CMD_ROWS},
    {"distinct", "distinct [COLUMN]", "drop repeated rows, keeping the first of each", &cmd_distinct, "", CMD_ROWS},
    {"tally", "tally [file...]  by: COLUMN", "count rows, or lines, words, characters and bytes of text", &cmd_tally, "by", CMD_ANY},
};

constexpr helm::Table TABLE{"rows", COMMANDS, sizeof(COMMANDS) / sizeof(COMMANDS[0])};

}

void register_row_commands() { register_table(&TABLE); }

}
