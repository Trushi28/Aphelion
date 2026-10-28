#include <cosmos/helm_rows.hpp>
#include <cosmos/kheap.hpp>
#include <cosmos/civil.hpp>

namespace helm {

namespace {

constexpr u32 ARENA_BYTES = 16384;
constexpr u32 MAX_ARGS = 256;
constexpr u64 CAPTURE_LIMIT = 8ull << 20;
constexpr u32 MAX_CANDIDATES = 128;
constexpr u32 CANDIDATE_BYTES = 60;

const Table* g_tables[MAX_TABLES];
u32 g_table_count;

bool same(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

u32 length(const char* s) { return static_cast<u32>(cstr::length(s)); }

const char* next_cp(const char* s) {
    ++s;
    while ((static_cast<u8>(*s) & 0xC0) == 0x80) ++s;
    return s;
}

struct Capture {
    u8* data;
    u64 size;
    u64 cap;
    bool overflow;
};

void capture_write(void* ctx, const char* d, u32 n) {
    Capture& c = *static_cast<Capture*>(ctx);
    if (c.overflow) return;
    if (c.size + n > CAPTURE_LIMIT) { c.overflow = true; return; }
    if (c.size + n > c.cap) {
        u64 cap = c.cap ? c.cap * 2 : 4096;
        while (cap < c.size + n) cap *= 2;
        u8* bigger = static_cast<u8*>(kheap::alloc(static_cast<usize>(cap)));
        if (!bigger) { c.overflow = true; return; }
        if (c.size) mem::copy(bigger, c.data, static_cast<usize>(c.size));
        if (c.data) kheap::free(c.data);
        c.data = bigger;
        c.cap = cap;
    }
    mem::copy(c.data + c.size, d, n);
    c.size += n;
}

void capture_free(Capture& c) {
    if (c.data) kheap::free(c.data);
    c = Capture{};
}

struct ArgSet {
    char* arena;
    u32 used;
    char** argv;
    u32 argc;
};

bool add_arg(ArgSet& a, const char* text, u32 len) {
    if (a.argc >= MAX_ARGS || a.used + len + 1 > ARENA_BYTES) return false;
    char* dst = a.arena + a.used;
    mem::copy(dst, text, len);
    dst[len] = 0;
    a.used += len + 1;
    a.argv[a.argc++] = dst;
    return true;
}

struct Work {
    parse::Parsed parsed;
    char arena[ARENA_BYTES];
    char* argv[MAX_ARGS];
};

void strip_marks(char* s) {
    for (; *s; ++s) {
        if (*s == parse::MARK_STAR) *s = '*';
        else if (*s == parse::MARK_ANY) *s = '?';
    }
}

bool has_marks(const char* s) {
    for (; *s; ++s)
        if (*s == parse::MARK_STAR || *s == parse::MARK_ANY) return true;
    return false;
}

bool expand_word(Shell& sh, const char* raw, char* out, u32 cap) {
    u32 n = 0;
    auto emit = [&](char c) -> bool {
        if (n + 1 >= cap) return false;
        out[n++] = c;
        return true;
    };
    for (u32 i = 0; raw[i]; ++i) {
        char c = raw[i];
        if (c != parse::MARK_VAR) {
            if (!emit(c)) return false;
            continue;
        }
        char name[VAR_NAME + 1];
        u32 k = 0;
        ++i;
        while (raw[i] && raw[i] != parse::MARK_END) {
            if (k < VAR_NAME) name[k++] = raw[i];
            ++i;
        }
        name[k] = 0;
        char tmp[nebula::path::TEXT_MAX];
        const char* value = nullptr;
        if (same(name, "status")) {
            u32 t = 0;
            int v = sh.status;
            char d[12];
            u32 dl = 0;
            if (v <= 0) d[dl++] = '0';
            while (v > 0) { d[dl++] = static_cast<char>('0' + v % 10); v /= 10; }
            while (dl) tmp[t++] = d[--dl];
            tmp[t] = 0;
            value = tmp;
        } else if (same(name, "here")) {
            lens::text::location_text(sh.cursor, tmp, sizeof(tmp));
            value = tmp;
        } else if ((value = var_get(sh, name)) == nullptr) {
            value = "";
        }
        for (u32 v = 0; value[v]; ++v)
            if (!emit(value[v])) return false;
    }
    out[n] = 0;
    return true;
}

nebula::Status glob_expand(Shell& sh, const char* pattern, ArgSet& args, Io* io, bool* none) {
    *none = false;
    u32 len = length(pattern);
    u32 first_mark = len;
    for (u32 i = 0; i < len; ++i)
        if (pattern[i] == parse::MARK_STAR || pattern[i] == parse::MARK_ANY) { first_mark = i; break; }
    i64 last_slash = -1;
    for (u32 i = 0; i < len; ++i)
        if (pattern[i] == '/') last_slash = i;
    u32 dir_len = 0;
    if (last_slash >= 0) {
        dir_len = static_cast<u32>(last_slash) + 1;
    } else {
        for (u32 i = 0; i < len && pattern[i] != '/'; ++i)
            if (pattern[i] == ':') { dir_len = i + 1; break; }
    }
    if (first_mark < dir_len) return nebula::Status::InvalidPath;

    char dir[nebula::path::TEXT_MAX];
    mem::copy(dir, pattern, dir_len);
    dir[dir_len] = 0;
    const char* leaf = pattern + dir_len;

    nebula::Listing l;
    nebula::Status st = lens::text::list(sh.cursor, dir_len ? dir : ".", &l, false);
    if (st != nebula::Status::Ok) return st;
    u32 matched = 0;
    nebula::Status result = nebula::Status::Ok;
    for (u32 i = 0; i < l.count; ++i) {
        if (!glob_match(leaf, l.items[i].name)) continue;
        char full[nebula::path::TEXT_MAX];
        u32 nl = length(l.items[i].name);
        if (dir_len + nl + 1 >= sizeof(full)) { result = nebula::Status::InvalidPath; break; }
        mem::copy(full, dir, dir_len);
        mem::copy(full + dir_len, l.items[i].name, nl);
        full[dir_len + nl] = 0;
        if (!add_arg(args, full, dir_len + nl)) { result = nebula::Status::TooLarge; break; }
        ++matched;
    }
    nebula::release(l);
    (void)io;
    if (result == nebula::Status::Ok && matched == 0) *none = true;
    return result;
}

constexpr u32 MAX_NAMED = 8;

struct Expanded {
    ArgSet args;
    Named named[MAX_NAMED];
    u32 named_count;
    const Command* cmd;
};

bool has_key(const char* keys, const char* key, u32 klen) {
    const char* p = keys;
    while (*p) {
        const char* e = p;
        while (*e && *e != ' ') ++e;
        if (static_cast<u32>(e - p) == klen && mem::compare(p, key, klen) == 0) return true;
        p = *e ? e + 1 : e;
    }
    return false;
}

bool is_key_word(const char* raw, u32 len) {
    if (len < 2 || raw[len - 1] != ':') return false;
    if (raw[0] < 'a' || raw[0] > 'z') return false;
    for (u32 i = 0; i + 1 < len; ++i) {
        char c = raw[i];
        if (!((c >= 'a' && c <= 'z') || c == '-')) return false;
    }
    return true;
}

void warn(Writer& err, const char* a, const char* b = nullptr, const char* c = nullptr, const char* d = nullptr) {
    err.rgb(palette::SAND);
    err.str("  ");
    err.str(a);
    if (b) err.str(b);
    if (c) err.str(c);
    if (d) err.str(d);
    err.str("\n");
    err.reset();
}

char* arena_put(ArgSet& a, const char* text, u32 len) {
    if (a.used + len + 1 > ARENA_BYTES) return nullptr;
    char* dst = a.arena + a.used;
    mem::copy(dst, text, len);
    dst[len] = 0;
    a.used += len + 1;
    return dst;
}

void list_keys(const char* keys, char* out, u32 cap) {
    u32 n = 0;
    const char* p = keys;
    while (*p && n + 2 < cap) {
        while (*p && *p != ' ' && n + 2 < cap) out[n++] = *p++;
        out[n++] = ':';
        if (*p == ' ') { out[n++] = ' '; ++p; }
    }
    out[n] = 0;
}

bool expand_stage(Shell& sh, const parse::Parsed& p, u32 stage, Expanded& ex, Writer& err, int* status) {
    ex.args.used = 0;
    ex.args.argc = 0;
    ex.named_count = 0;
    ex.cmd = nullptr;
    *status = 1;
    u32 total = p.stages[stage].args;
    u32 first = 0;
    for (u32 i = 0; i < p.word_count; ++i)
        if (p.words[i].stage == stage) { first = i; break; }

    char buf[nebula::path::TEXT_MAX + 64];
    if (!expand_word(sh, p.text(p.words[first]), buf, sizeof(buf))) {
        warn(err, "word is too long after expansion");
        return false;
    }
    if (has_marks(buf)) {
        warn(err, "a command name cannot contain wildcards");
        return false;
    }
    const Command* cmd = find_command(buf);
    if (!cmd) {
        err.rgb(palette::SAND);
        err.str("  unknown command: ");
        err.str(buf);
        err.reset();
        err.str("\n  try ");
        err.rgb(palette::ICE);
        err.str("help");
        err.reset();
        err.put('\n');
        *status = 127;
        return false;
    }
    ex.cmd = cmd;
    if (!add_arg(ex.args, buf, length(buf))) {
        warn(err, "too many or too long arguments");
        return false;
    }

    for (u32 k = 1; k < total; ++k) {
        const parse::Word& w = p.words[first + k];
        const char* raw = p.text(w);
        if (!w.quoted && is_key_word(raw, w.len)) {
            u32 klen = w.len - 1;
            char key[VAR_NAME + 8];
            if (klen < sizeof(key)) {
                mem::copy(key, raw, klen);
                key[klen] = 0;
            }
            if (klen < sizeof(key) && has_key(cmd->keys, raw, klen)) {
                if (k + 1 >= total) {
                    warn(err, raw, " needs a value");
                    return false;
                }
                for (u32 n = 0; n < ex.named_count; ++n) {
                    if (same(ex.named[n].key, key)) {
                        warn(err, raw, " was given twice");
                        return false;
                    }
                }
                if (ex.named_count >= MAX_NAMED) {
                    warn(err, "too many options");
                    return false;
                }
                const parse::Word& vw = p.words[first + k + 1];
                char vbuf[nebula::path::TEXT_MAX + 64];
                if (!expand_word(sh, p.text(vw), vbuf, sizeof(vbuf))) {
                    warn(err, "word is too long after expansion");
                    return false;
                }
                strip_marks(vbuf);
                char* kp = arena_put(ex.args, key, klen);
                char* vp = arena_put(ex.args, vbuf, length(vbuf));
                if (!kp || !vp) {
                    warn(err, "too many or too long arguments");
                    return false;
                }
                ex.named[ex.named_count++] = Named{kp, vp};
                ++k;
                continue;
            }
            if (klen < sizeof(key) && !nebula::find_realm(key)) {
                char known[160];
                list_keys(cmd->keys, known, sizeof(known));
                err.rgb(palette::SAND);
                err.str("  ");
                err.str(cmd->name);
                err.str(" does not take the option ");
                err.str(raw);
                if (known[0]) { err.str("; its options: "); err.str(known); }
                else err.str("; it takes none");
                err.str(" (quote the word to use it as plain text)\n");
                err.reset();
                return false;
            }
        }
        if (!expand_word(sh, raw, buf, sizeof(buf))) {
            warn(err, "word is too long after expansion");
            return false;
        }
        if (cmd->flags & CMD_LITERAL) {
            strip_marks(buf);
            if (!add_arg(ex.args, buf, length(buf))) {
                warn(err, "too many or too long arguments");
                return false;
            }
        } else if (has_marks(buf)) {
            bool none = false;
            nebula::Status st = glob_expand(sh, buf, ex.args, nullptr, &none);
            if (st != nebula::Status::Ok || none) {
                char shown[nebula::path::TEXT_MAX + 64];
                mem::copy(shown, buf, length(buf) + 1);
                strip_marks(shown);
                err.rgb(palette::SAND);
                if (st == nebula::Status::InvalidPath) err.str("  wildcards are only allowed in the last path component: ");
                else if (none) err.str("  no match: ");
                else err.str("  cannot expand ");
                err.str(shown);
                if (!none && st != nebula::Status::InvalidPath) { err.str(": "); err.str(nebula::status_text(st)); }
                err.str("\n");
                err.reset();
                return false;
            }
        } else if (!add_arg(ex.args, buf, length(buf))) {
            warn(err, "too many or too long arguments");
            return false;
        }
    }
    return true;
}

struct Flow {
    enum Kind : u8 { None, Bytes, Rows };
    Kind kind;
    Capture bytes;
    rows::Table table;
};

void flow_init(Flow& f) {
    f.kind = Flow::None;
    f.bytes = Capture{};
    rows::begin(f.table);
}

void flow_clear(Flow& f) {
    capture_free(f.bytes);
    rows::release(f.table);
    f.kind = Flow::None;
}

int run_pipe(Shell& sh, Work& w, const parse::Pipe& pipe) {
    Flow carry;
    flow_init(carry);
    int status = 0;
    Writer err(sh.terminal, sh.color, true);

    for (u32 k = 0; k < pipe.stage_count; ++k) {
        u32 stage = pipe.first_stage + k;
        bool last = k + 1 == pipe.stage_count;
        Expanded ex{};
        ex.args = ArgSet{w.arena, 0, w.argv, 0};
        int failed = 1;
        if (!expand_stage(sh, w.parsed, stage, ex, err, &failed)) { status = failed; break; }
        const Command* cmd = ex.cmd;

        Capture flat{};
        rows::Table from_bytes;
        rows::begin(from_bytes);
        const u8* in_ptr = nullptr;
        u64 in_size = 0;
        bool has_in = false;
        const rows::Table* in_rows = nullptr;
        bool input_failed = false;
        if (carry.kind == Flow::Rows) {
            if (cmd->flags & (CMD_ROWS | CMD_ANY)) {
                in_rows = &carry.table;
            } else {
                {
                    Writer fw(Sink{&capture_write, &flat}, false, false);
                    rows::flatten(fw, carry.table);
                }
                if (flat.overflow) {
                    warn(err, "pipe buffer exceeded 8 MiB (or memory ran out)");
                    input_failed = true;
                } else {
                    in_ptr = flat.data;
                    in_size = flat.size;
                    has_in = true;
                }
            }
        } else if (carry.kind == Flow::Bytes) {
            if (cmd->flags & CMD_ROWS) {
                if (!rows::from_text(from_bytes, carry.bytes.data, carry.bytes.size)) {
                    warn(err, "too many lines or out of memory");
                    input_failed = true;
                } else {
                    in_rows = &from_bytes;
                }
            } else {
                in_ptr = carry.bytes.data;
                in_size = carry.bytes.size;
                has_in = true;
            }
        }
        if (input_failed) {
            capture_free(flat);
            rows::release(from_bytes);
            status = 1;
            break;
        }

        rows::Table produced;
        rows::begin(produced);
        Capture cap{};
        {
            Sink s = last ? sh.terminal : Sink{&capture_write, &cap};
            Writer out(s, last && sh.color, last);
            Io io{out, err, in_ptr, in_size, has_in, in_rows, &produced, ex.named, ex.named_count};
            status = cmd->fn(sh, io, ex.args.argc, ex.args.argv);
        }
        err.flush();
        capture_free(flat);
        rows::release(from_bytes);
        flow_clear(carry);

        if (sh.aborted) {
            capture_free(cap);
            rows::release(produced);
            warn(err, "interrupted");
            status = 130;
            break;
        }
        bool have_rows = produced.cols > 0 && !produced.failed;
        if (last) {
            if (have_rows) {
                Writer shown(sh.terminal, sh.color, true);
                rows::render(shown, produced);
            }
            capture_free(cap);
            rows::release(produced);
        } else if (have_rows) {
            carry.table = produced;
            carry.kind = Flow::Rows;
            rows::begin(produced);
            capture_free(cap);
        } else {
            rows::release(produced);
            if (cap.overflow) {
                capture_free(cap);
                warn(err, "pipe buffer exceeded 8 MiB (or memory ran out)");
                status = 1;
                break;
            }
            carry.bytes = cap;
            carry.kind = Flow::Bytes;
        }
    }
    flow_clear(carry);
    return status;
}

}

bool register_table(const Table* table) {
    for (u32 i = 0; i < g_table_count; ++i)
        if (g_tables[i] == table) return true;
    if (g_table_count >= MAX_TABLES) return false;
    g_tables[g_table_count++] = table;
    return true;
}

const Command* find_command(const char* name) {
    for (u32 t = 0; t < g_table_count; ++t)
        for (u32 i = 0; i < g_tables[t]->count; ++i)
            if (same(g_tables[t]->commands[i].name, name)) return &g_tables[t]->commands[i];
    return nullptr;
}

u32 table_count() { return g_table_count; }
const Table* table_at(u32 index) { return index < g_table_count ? g_tables[index] : nullptr; }

void init(Shell& sh, Sink terminal, bool color, u32 cols, const char* start_realm) {
    mem::zero(&sh, sizeof(sh));
    sh.terminal = terminal;
    sh.color = color;
    sh.cols = cols ? cols : 80;
    (void)lens::text::init(sh.cursor, start_realm);
}

void shutdown(Shell& sh) { lens::text::close(sh.cursor); }

const char* var_get(const Shell& sh, const char* name) {
    for (u32 i = 0; i < MAX_VARS; ++i)
        if (sh.vars[i].used && same(sh.vars[i].name, name)) return sh.vars[i].value;
    return nullptr;
}

bool var_reserved(const char* name) { return same(name, "status") || same(name, "here"); }

bool var_set(Shell& sh, const char* name, const char* value) {
    u32 nl = length(name);
    if (nl == 0 || nl >= VAR_NAME || length(value) >= VAR_VALUE || var_reserved(name)) return false;
    if (name[0] >= '0' && name[0] <= '9') return false;
    for (u32 i = 0; i < nl; ++i) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    Var* slot = nullptr;
    for (u32 i = 0; i < MAX_VARS; ++i) {
        if (sh.vars[i].used && same(sh.vars[i].name, name)) { slot = &sh.vars[i]; break; }
        if (!sh.vars[i].used && !slot) slot = &sh.vars[i];
    }
    if (!slot) return false;
    slot->used = true;
    mem::copy(slot->name, name, nl + 1);
    mem::copy(slot->value, value, length(value) + 1);
    return true;
}

bool var_unset(Shell& sh, const char* name) {
    for (u32 i = 0; i < MAX_VARS; ++i)
        if (sh.vars[i].used && same(sh.vars[i].name, name)) { sh.vars[i] = Var{}; return true; }
    return false;
}

bool glob_match(const char* p, const char* s) {
    const char* star_p = nullptr;
    const char* star_s = nullptr;
    while (*s) {
        if (*p == parse::MARK_STAR) { star_p = ++p; star_s = s; continue; }
        if (*p == parse::MARK_ANY) { ++p; s = next_cp(s); continue; }
        if (*p && *p == *s) { ++p; ++s; continue; }
        if (star_p) { star_s = next_cp(star_s); s = star_s; p = star_p; continue; }
        return false;
    }
    while (*p == parse::MARK_STAR) ++p;
    return *p == 0;
}

void mark_wildcards(const char* in, char* out, u32 cap) {
    u32 n = 0;
    for (; *in && n + 1 < cap; ++in) {
        char c = *in;
        if (c == '*') c = parse::MARK_STAR;
        else if (c == '?') c = parse::MARK_ANY;
        out[n++] = c;
    }
    out[n] = 0;
}

const char* opt(const Io& io, const char* key) {
    for (u32 i = 0; i < io.named_count; ++i)
        if (same(io.named[i].key, key)) return io.named[i].value;
    return nullptr;
}

Flag opt_flag(const Io& io, const char* key) {
    const char* v = opt(io, key);
    if (!v) return Flag::Absent;
    u64 parsed = 0;
    if (!rows::parse_value(rows::Kind::Flag, v, &parsed)) return Flag::Bad;
    return parsed ? Flag::Yes : Flag::No;
}

bool opt_yes(const Io& io, const char* key) { return opt_flag(io, key) == Flag::Yes; }

bool opt_u64(const Io& io, const char* key, u64* out) {
    const char* v = opt(io, key);
    return v && parse_u64(v, out);
}

bool opt_bool(Io& io, const char* key, bool* out) {
    Flag f = opt_flag(io, key);
    if (f == Flag::Bad) {
        io.err.rgb(palette::SAND);
        io.err.str("  ");
        io.err.str(key);
        io.err.str(": takes yes or no\n");
        io.err.reset();
        return false;
    }
    if (f != Flag::Absent) *out = f == Flag::Yes;
    return true;
}

bool opt_num(Io& io, const char* key, u64* out) {
    const char* v = opt(io, key);
    if (!v) return true;
    if (!parse_u64(v, out)) {
        io.err.rgb(palette::SAND);
        io.err.str("  ");
        io.err.str(key);
        io.err.str(": takes a whole number\n");
        io.err.reset();
        return false;
    }
    return true;
}

bool parse_u64(const char* s, u64* out) {
    if (!*s) return false;
    u64 v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && s[2]) {
        for (s += 2; *s; ++s) {
            int d = (*s >= '0' && *s <= '9') ? *s - '0' : (*s >= 'a' && *s <= 'f') ? *s - 'a' + 10 : (*s >= 'A' && *s <= 'F') ? *s - 'A' + 10 : -1;
            if (d < 0 || v > (~0ull >> 4)) return false;
            v = (v << 4) | static_cast<u64>(d);
        }
        *out = v;
        return true;
    }
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return false;
        u64 d = static_cast<u64>(*s - '0');
        if (v > (~0ull - d) / 10) return false;
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

void print_error(Io& io, const char* message) {
    io.err.rgb(palette::SAND);
    io.err.str("  ");
    io.err.str(message);
    io.err.str("\n");
    io.err.reset();
}

void print_status(Io& io, const char* what, const char* subject, nebula::Status st) {
    io.err.rgb(palette::SAND);
    io.err.str("  ");
    io.err.str(what);
    io.err.put(' ');
    io.err.str(subject);
    io.err.str(": ");
    io.err.str(nebula::status_text(st));
    io.err.str("\n");
    io.err.reset();
}

int code_of(nebula::Status st) { return st == nebula::Status::Ok ? 0 : 1; }

void write_safe(Writer& w, const u8* data, u64 size) {
    utf8::Decoder d;
    u32 cps[2];
    for (u64 i = 0; i < size; ++i) {
        u32 k = d.feed(data[i], cps);
        for (u32 j = 0; j < k; ++j) {
            u32 cp = cps[j];
            if (cp == '\n' || cp == '\t') w.put(static_cast<char>(cp));
            else if (cp == '\r') { if (!(i + 1 < size && data[i + 1] == '\n')) w.cp(0x240D); }
            else if (cp < 0x20) w.cp(0x2400 + cp);
            else if (cp == 0x7F) w.cp(0x2421);
            else if (cp >= 0x80 && cp < 0xA0) w.cp(utf8::REPLACEMENT);
            else w.cp(cp);
        }
    }
    u32 tail[1];
    if (d.finish(tail)) w.cp(utf8::REPLACEMENT);
}

int for_each_input(Shell& sh, Io& io, u32 argc, char** argv, u32 first, InputFn fn, void* ctx) {
    int status = 0;
    if (first >= argc) {
        if (!io.has_in) {
            print_error(io, "no input: name a file or pipe something in");
            return 1;
        }
        fn(nullptr, io.in, io.in_size, ctx);
        return 0;
    }
    for (u32 i = first; i < argc; ++i) {
        if (sh.poll()) return 130;
        if (same(argv[i], "-")) {
            if (!io.has_in) { print_error(io, "no input on the pipe"); status = 1; continue; }
            fn(nullptr, io.in, io.in_size, ctx);
            continue;
        }
        nebula::Blob b;
        nebula::Status st = lens::text::read_file(sh.cursor, argv[i], &b);
        if (st != nebula::Status::Ok) {
            print_status(io, "cannot read", argv[i], st);
            status = 1;
            continue;
        }
        fn(argv[i], b.data, b.size, ctx);
        nebula::release(b);
    }
    return status;
}

void format_time(u64 ms, char* out) {
    u64 secs = ms / 1000;
    if (secs < 946684800ull) {
        const char* dash = "----------------";
        for (u32 i = 0; i < 17; ++i) out[i] = dash[i];
        return;
    }
    civil::Date d = civil::from_unix(secs);
    char iso[24];
    civil::format_iso(d, iso);
    for (u32 i = 0; i < 10; ++i) out[i] = iso[i];
    out[10] = ' ';
    for (u32 i = 0; i < 5; ++i) out[11 + i] = iso[11 + i];
    out[16] = 0;
}

void print_size(Writer& w, u64 bytes) {
    if (bytes < 1024) { w.num(bytes); w.str(" B"); return; }
    const char* units[] = {"KiB", "MiB", "GiB", "TiB"};
    u32 u = 0;
    u64 whole = bytes;
    u64 rem = 0;
    while (whole >= 1024 && u < 3) { rem = whole % 1024; whole /= 1024; ++u; }
    w.num(whole);
    w.put('.');
    w.num(rem * 10 / 1024);
    w.put(' ');
    w.str(units[u - 1]);
}

int run_line(Shell& sh, const char* line) {
    Work* w = static_cast<Work*>(kheap::alloc(sizeof(Work)));
    Writer err(sh.terminal, sh.color, true);
    if (!w) {
        err.rgb(palette::SAND);
        err.str("  out of memory\n");
        err.reset();
        sh.status = 1;
        return 1;
    }
    sh.aborted = false;
    parse::Error pe = parse::parse(line, w->parsed);
    if (pe != parse::Error::Ok) {
        err.rgb(palette::SAND);
        err.str("  syntax: ");
        err.str(parse::describe(pe));
        if (pe != parse::Error::TooLong && pe != parse::Error::BadText) {
            err.str(" (byte ");
            err.num(w->parsed.error_at + 1);
            err.str(")");
        }
        err.str("\n");
        err.reset();
        kheap::free(w);
        sh.status = 2;
        return 2;
    }
    for (u32 i = 0; i < w->parsed.pipe_count && !sh.quit; ++i) {
        const parse::Pipe& pipe = w->parsed.pipes[i];
        if (pipe.link == parse::Link::Then && sh.status != 0) continue;
        if (pipe.link == parse::Link::Else && sh.status == 0) continue;
        sh.status = run_pipe(sh, *w, pipe);
        if (sh.aborted) break;
    }
    kheap::free(w);
    return sh.status;
}

namespace {

struct Candidates {
    char (*items)[CANDIDATE_BYTES];
    u32 count;
    bool truncated;
    bool failed;
};

void candidate_add(Candidates& c, const char* name, bool dir, const char* suffix = nullptr) {
    if (c.count >= MAX_CANDIDATES) { c.truncated = true; return; }
    u32 n = length(name);
    u32 extra = dir ? 1 : 0;
    u32 sl = suffix ? length(suffix) : 0;
    if (n + extra + sl + 1 > CANDIDATE_BYTES) { c.truncated = true; return; }
    char* dst = c.items[c.count++];
    mem::copy(dst, name, n);
    u32 p = n;
    if (suffix) { mem::copy(dst + p, suffix, sl); p += sl; }
    if (dir) dst[p++] = '/';
    dst[p] = 0;
}

bool less(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return static_cast<u8>(*a) < static_cast<u8>(*b);
}

bool has_prefix(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s != *prefix) return false;
        ++s;
        ++prefix;
    }
    return true;
}

bool needs_escape(char c, char quote) {
    if (quote == '\'') return false;
    if (quote == '"') return c == '"' || c == '\\' || c == '@';
    switch (c) {
        case ' ': case '\t': case '\\': case '"': case '\'': case '@': case '#': case '*': case '?':
        case ';': case '|':
            return true;
        default:
            return false;
    }
}

}

bool complete(Shell& sh, const char* line, u32 cursor, Completion* out) {
    mem::zero(out, sizeof(*out));
    u32 word_start = 0;
    bool cmd_pos = true;
    bool in_word = false;
    char quote = 0;
    bool escaped = false;
    for (u32 i = 0; i < cursor; ++i) {
        char c = line[i];
        if (escaped) { escaped = false; continue; }
        if (quote) {
            if (c == quote) quote = 0;
            else if (quote == '"' && c == '\\') escaped = true;
            continue;
        }
        if (c == '\\') { escaped = true; in_word = true; continue; }
        if (c == '\'' || c == '"') { quote = c; in_word = true; continue; }
        if (c == ' ' || c == '\t') {
            if (in_word) cmd_pos = false;
            in_word = false;
            word_start = i + 1;
            continue;
        }
        if (c == ';' || c == '|') { cmd_pos = true; in_word = false; word_start = i + 1; continue; }
        if (!in_word && (c == '=' || c == '!') && i + 2 <= cursor && line[i + 1] == '>' &&
            (i + 2 == cursor || line[i + 2] == ' ' || line[i + 2] == '\t')) {
            cmd_pos = true;
            word_start = i + 2;
            ++i;
            continue;
        }
        in_word = true;
    }
    if (!in_word) word_start = cursor;

    char typed[nebula::path::TEXT_MAX];
    u32 tn = 0;
    char opened = 0;
    {
        char q = 0;
        bool esc = false;
        for (u32 i = word_start; i < cursor; ++i) {
            char c = line[i];
            if (esc) { if (tn + 1 < sizeof(typed)) typed[tn++] = c; esc = false; continue; }
            if (q) {
                if (c == q) { q = 0; continue; }
                if (q == '"' && c == '\\') { esc = true; continue; }
                if (tn + 1 < sizeof(typed)) typed[tn++] = c;
                continue;
            }
            if (c == '\\') { esc = true; continue; }
            if (c == '\'' || c == '"') { q = c; if (i == word_start) opened = c; continue; }
            if (tn + 1 < sizeof(typed)) typed[tn++] = c;
        }
        if (q) opened = q;
        else if (opened) opened = 0;
    }
    typed[tn] = 0;

    Candidates cand{};
    cand.items = static_cast<char (*)[CANDIDATE_BYTES]>(kheap::alloc(sizeof(char[CANDIDATE_BYTES]) * MAX_CANDIDATES));
    if (!cand.items) return false;

    bool slash_or_colon = false;
    for (u32 i = 0; i < tn; ++i)
        if (typed[i] == '/' || typed[i] == ':') slash_or_colon = true;

    u32 dir_len = 0;
    bool is_command = cmd_pos && !slash_or_colon;
    const char* leaf = typed;
    if (is_command) {
        for (u32 t = 0; t < table_count(); ++t) {
            const Table* tb = table_at(t);
            for (u32 i = 0; i < tb->count; ++i)
                if (has_prefix(tb->commands[i].name, typed)) candidate_add(cand, tb->commands[i].name, false);
        }
        for (u32 i = 1; i < cand.count; ++i) {
            char tmp[CANDIDATE_BYTES];
            mem::copy(tmp, cand.items[i], CANDIDATE_BYTES);
            u32 j = i;
            while (j > 0 && less(tmp, cand.items[j - 1])) {
                mem::copy(cand.items[j], cand.items[j - 1], CANDIDATE_BYTES);
                --j;
            }
            mem::copy(cand.items[j], tmp, CANDIDATE_BYTES);
        }
    } else {
        i64 last_slash = -1;
        for (u32 i = 0; i < tn; ++i)
            if (typed[i] == '/') last_slash = i;
        if (last_slash >= 0) dir_len = static_cast<u32>(last_slash) + 1;
        else {
            for (u32 i = 0; i < tn; ++i)
                if (typed[i] == ':') { dir_len = i + 1; break; }
        }
        leaf = typed + dir_len;
        char dir[nebula::path::TEXT_MAX];
        mem::copy(dir, typed, dir_len);
        dir[dir_len] = 0;
        if (dir_len == 0) {
            for (u32 r = 0; r < nebula::realm_count(); ++r) {
                const char* rn = nebula::realm_at(r)->name();
                if (has_prefix(rn, leaf)) candidate_add(cand, rn, true, ":");
            }
        }
        nebula::Listing l;
        if (lens::text::list(sh.cursor, dir_len ? dir : ".", &l, false) == nebula::Status::Ok) {
            for (u32 i = 0; i < l.count; ++i)
                if (has_prefix(l.items[i].name, leaf)) candidate_add(cand, l.items[i].name, l.items[i].kind == nebula::Kind::Dir);
            nebula::release(l);
        }
    }

    if (cand.count == 0) { kheap::free(cand.items); return false; }

    u32 lcp = length(cand.items[0]);
    for (u32 i = 1; i < cand.count; ++i) {
        u32 k = 0;
        while (k < lcp && cand.items[i][k] == cand.items[0][k]) ++k;
        lcp = k;
    }
    u32 leaf_len = length(leaf);
    if (cand.count > 1) {
        while (lcp > leaf_len && (static_cast<u8>(cand.items[0][lcp]) & 0xC0) == 0x80) --lcp;
    }

    char merged[nebula::path::TEXT_MAX + CANDIDATE_BYTES];
    u32 mn = 0;
    for (u32 i = 0; i < dir_len; ++i) merged[mn++] = typed[i];
    for (u32 i = 0; i < lcp; ++i) merged[mn++] = cand.items[0][i];
    merged[mn] = 0;

    u32 pos = 0;
    bool ok = true;
    if (opened) out->insert[pos++] = opened;
    for (u32 i = 0; i < mn && ok; ++i) {
        char c = merged[i];
        if (opened == '\'' && c == '\'') { ok = false; break; }
        if (needs_escape(c, opened)) {
            if (pos + 2 >= sizeof(out->insert)) { ok = false; break; }
            out->insert[pos++] = '\\';
        }
        if (pos + 1 >= sizeof(out->insert)) { ok = false; break; }
        out->insert[pos++] = c;
    }
    out->unique = cand.count == 1;
    if (ok && out->unique) {
        bool dir = merged[mn - 1] == '/';
        if (!dir) {
            if (opened && pos + 1 < sizeof(out->insert)) out->insert[pos++] = opened;
            if (pos + 1 < sizeof(out->insert)) out->insert[pos++] = ' ';
        }
    }
    out->insert[pos] = 0;
    out->start = word_start;
    out->count = cand.count;
    out->is_command = is_command;

    u32 total = 0;
    for (u32 i = 0; i < cand.count; ++i) total += length(cand.items[i]) + 1;
    out->names = static_cast<char*>(kheap::alloc(total));
    if (out->names) {
        u32 p = 0;
        for (u32 i = 0; i < cand.count; ++i) {
            u32 n = length(cand.items[i]);
            mem::copy(out->names + p, cand.items[i], n + 1);
            p += n + 1;
        }
        out->names_bytes = total;
    }
    kheap::free(cand.items);
    if (!ok) { release(*out); return false; }
    return true;
}

void release(Completion& c) {
    if (c.names) kheap::free(c.names);
    c.names = nullptr;
    c.names_bytes = 0;
}

}
