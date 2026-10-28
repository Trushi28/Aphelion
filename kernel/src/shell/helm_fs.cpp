#include <cosmos/helm_rows.hpp>
#include <cosmos/nebula_realms.hpp>
#include <cosmos/blockdev.hpp>
#include <cosmos/kheap.hpp>

namespace helm {

namespace {

using rows::Kind;

bool same(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

u32 length(const char* s) { return static_cast<u32>(cstr::length(s)); }

int usage(Io& io, const char* text) {
    print_error(io, text);
    return 2;
}

bool too_many(Io& io, rows::Table& t) {
    if (!t.failed) return false;
    print_error(io, "too many rows or out of memory");
    return true;
}

int cmd_here(Shell& sh, Io& io, u32 argc, char**) {
    if (argc != 1) return usage(io, "usage: here");
    char loc[nebula::path::TEXT_MAX];
    lens::text::location_text(sh.cursor, loc, sizeof(loc));
    io.out.str(loc);
    io.out.put('\n');
    return 0;
}

int cmd_go(Shell& sh, Io& io, u32 argc, char** argv) {
    if (argc > 2) return usage(io, "usage: go [path]   (no path goes to the root of the current realm; back returns to the last place)");
    char target[nebula::path::TEXT_MAX];
    nebula::path::Path before = sh.cursor.here;
    if (argc == 1) {
        u32 n = length(sh.cursor.here.realm);
        mem::copy(target, sh.cursor.here.realm, n);
        target[n] = ':';
        target[n + 1] = 0;
    } else {
        u32 n = length(argv[1]);
        if (n >= sizeof(target)) { print_error(io, "path is too long"); return 1; }
        mem::copy(target, argv[1], n + 1);
    }
    nebula::Status st = lens::text::change_location(sh.cursor, target);
    if (st != nebula::Status::Ok) { print_status(io, "cannot go to", target, st); return 1; }
    sh.previous = before;
    sh.has_previous = true;
    return 0;
}

int cmd_back(Shell& sh, Io& io, u32 argc, char**) {
    if (argc != 1) return usage(io, "usage: back");
    if (!sh.has_previous) { print_error(io, "there is no previous location"); return 1; }
    char target[nebula::path::TEXT_MAX];
    nebula::path::Path before = sh.cursor.here;
    nebula::path::to_text(sh.previous, target, sizeof(target));
    nebula::Status st = lens::text::change_location(sh.cursor, target);
    if (st != nebula::Status::Ok) { print_status(io, "cannot go to", target, st); return 1; }
    sh.previous = before;
    sh.has_previous = true;
    return 0;
}

struct ChartCtx {
    Shell* sh;
    Io* io;
    rows::Table* out;
    char* path;
    u32 path_len;
    char* rel;
    u32 rel_len;
    u32 max_depth;
    int only;
    bool stop;
    bool failed;
};

void chart_row(ChartCtx& c, const char* name, u32 nlen, nebula::Kind kind, u64 size, u64 mtime) {
    u32 r = rows::add_row(*c.out);
    if (r == ~0u) { c.failed = true; c.stop = true; return; }
    rows::set_text(*c.out, r, 0, name, nlen);
    rows::set_text(*c.out, r, 1, kind == nebula::Kind::Dir ? "dir" : "file");
    if (kind == nebula::Kind::Dir) rows::set_null(*c.out, r, 2);
    else rows::set_num(*c.out, r, 2, size);
    rows::set_num(*c.out, r, 3, mtime);
    if (c.out->failed) { c.failed = true; c.stop = true; }
}

void chart_walk(ChartCtx& c, u32 depth) {
    nebula::Listing l;
    c.path[c.path_len] = 0;
    nebula::Status st = lens::text::list(c.sh->cursor, c.path, &l, true);
    if (st != nebula::Status::Ok) { print_status(*c.io, "cannot list", c.path, st); return; }
    for (u32 i = 0; i < l.count && !c.stop; ++i) {
        if (c.sh->poll()) { c.stop = true; break; }
        const nebula::Entry& e = l.items[i];
        bool is_dir = e.kind == nebula::Kind::Dir;
        u32 nl = length(e.name);
        if (c.path_len + 1 + nl + 1 >= nebula::path::TEXT_MAX || c.rel_len + 1 + nl + 1 >= nebula::path::TEXT_MAX) continue;
        u32 saved_path = c.path_len;
        u32 saved_rel = c.rel_len;
        if (c.path_len && c.path[c.path_len - 1] != '/') c.path[c.path_len++] = '/';
        mem::copy(c.path + c.path_len, e.name, nl);
        c.path_len += nl;
        if (c.rel_len) c.rel[c.rel_len++] = '/';
        mem::copy(c.rel + c.rel_len, e.name, nl);
        c.rel_len += nl;
        c.rel[c.rel_len] = 0;
        bool wanted = c.only == 0 || (c.only == 'd' && is_dir) || (c.only == 'f' && !is_dir);
        if (wanted) chart_row(c, c.rel, c.rel_len, e.kind, e.size, e.mtime_ms);
        if (is_dir && depth < c.max_depth && !c.stop) chart_walk(c, depth + 1);
        c.path_len = saved_path;
        c.rel_len = saved_rel;
    }
    nebula::release(l);
}

void chart_schema(rows::Table& t) {
    rows::begin(t);
    rows::add_column(t, "name", Kind::Text);
    rows::add_column(t, "kind", Kind::Text);
    rows::add_column(t, "size", Kind::Size);
    rows::add_column(t, "changed", Kind::Time);
    t.body = 0;
}

int cmd_chart(Shell& sh, Io& io, u32 argc, char** argv) {
    if (argc > 2) return usage(io, "usage: chart [path]  depth: N  only: dir|file");
    u64 depth = 1;
    if (!opt_num(io, "depth", &depth)) return 2;
    if (depth == 0 || depth > nebula::MAX_TREE_DEPTH) return usage(io, "chart: depth: takes a number from 1 to 16");
    int only = 0;
    if (const char* o = opt(io, "only")) {
        if (same(o, "dir")) only = 'd';
        else if (same(o, "file")) only = 'f';
        else return usage(io, "chart: only: takes dir or file");
    }
    const char* start = argc == 2 ? argv[1] : ".";
    nebula::Info inf;
    nebula::Status st = lens::text::stat(sh.cursor, start, &inf);
    if (st != nebula::Status::Ok) { print_status(io, "cannot chart", start, st); return 1; }
    rows::Table& out = *io.out_rows;
    chart_schema(out);
    ChartCtx c{};
    c.sh = &sh;
    c.io = &io;
    c.out = &out;
    c.only = only;
    c.max_depth = static_cast<u32>(depth);
    if (inf.kind != nebula::Kind::Dir) {
        if (only != 'd') chart_row(c, start, length(start), inf.kind, inf.size, inf.mtime_ms);
        return too_many(io, out) ? 1 : 0;
    }
    c.path = static_cast<char*>(kheap::alloc(nebula::path::TEXT_MAX));
    c.rel = static_cast<char*>(kheap::alloc(nebula::path::TEXT_MAX));
    if (!c.path || !c.rel) {
        if (c.path) kheap::free(c.path);
        if (c.rel) kheap::free(c.rel);
        print_error(io, "out of memory");
        return 1;
    }
    u32 n = length(start);
    if (n >= nebula::path::TEXT_MAX) {
        kheap::free(c.path);
        kheap::free(c.rel);
        print_error(io, "path is too long");
        return 1;
    }
    mem::copy(c.path, start, n + 1);
    c.path_len = n;
    c.rel[0] = 0;
    chart_walk(c, 1);
    kheap::free(c.path);
    kheap::free(c.rel);
    if (c.stop && !c.failed) return 130;
    return too_many(io, out) ? 1 : 0;
}

void print_entry_name(Writer& w, const char* name, nebula::Kind kind) {
    if (kind == nebula::Kind::Dir) {
        w.rgb(palette::ICE);
        w.str(name);
        if (w.terminal()) w.put('/');
        w.reset();
    } else {
        w.str(name);
    }
}

struct TreeState {
    char* path;
    u32 path_len;
    char prefix[16 * 8];
    u32 prefix_len;
    u32 max_depth;
    u64 dirs;
    u64 files;
    bool dirs_only;
    bool stop;
};

void tree_walk(Shell& sh, Io& io, TreeState& t, u32 depth) {
    if (depth >= t.max_depth) return;
    nebula::Listing l;
    t.path[t.path_len] = 0;
    nebula::Status st = lens::text::list(sh.cursor, t.path, &l, false);
    if (st != nebula::Status::Ok) {
        print_status(io, "cannot list", t.path, st);
        return;
    }
    u32 shown = 0;
    for (u32 i = 0; i < l.count; ++i)
        if (!t.dirs_only || l.items[i].kind == nebula::Kind::Dir) ++shown;
    u32 index = 0;
    for (u32 i = 0; i < l.count && !t.stop; ++i) {
        if (t.dirs_only && l.items[i].kind != nebula::Kind::Dir) continue;
        if (sh.poll()) { t.stop = true; break; }
        bool last = ++index == shown;
        io.out.rgb(palette::MIST);
        io.out.bytes(t.prefix, t.prefix_len);
        io.out.str(last ? "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 " : "\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 ");
        io.out.reset();
        print_entry_name(io.out, l.items[i].name, l.items[i].kind);
        io.out.put('\n');
        if (l.items[i].kind == nebula::Kind::Dir) {
            ++t.dirs;
            u32 saved_path = t.path_len;
            u32 saved_prefix = t.prefix_len;
            u32 nl = length(l.items[i].name);
            if (t.path_len + 1 + nl + 1 < nebula::path::TEXT_MAX && t.prefix_len + 7 <= sizeof(t.prefix)) {
                if (t.path_len && t.path[t.path_len - 1] != '/') t.path[t.path_len++] = '/';
                mem::copy(t.path + t.path_len, l.items[i].name, nl);
                t.path_len += nl;
                const char* seg = last ? "    " : "\xE2\x94\x82   ";
                u32 sl = length(seg);
                mem::copy(t.prefix + t.prefix_len, seg, sl);
                t.prefix_len += sl;
                tree_walk(sh, io, t, depth + 1);
            }
            t.path_len = saved_path;
            t.prefix_len = saved_prefix;
        } else {
            ++t.files;
        }
    }
    nebula::release(l);
}

int cmd_outline(Shell& sh, Io& io, u32 argc, char** argv) {
    if (argc > 2) return usage(io, "usage: outline [path]  depth: N  only: dir");
    u64 depth = 6;
    if (!opt_num(io, "depth", &depth)) return 2;
    if (depth == 0 || depth > 15) return usage(io, "outline: depth: takes a number from 1 to 15");
    bool dirs_only = false;
    if (const char* o = opt(io, "only")) {
        if (!same(o, "dir")) return usage(io, "outline: only: takes dir");
        dirs_only = true;
    }
    const char* start = argc == 2 ? argv[1] : ".";
    nebula::Info inf;
    nebula::Status st = lens::text::stat(sh.cursor, start, &inf);
    if (st != nebula::Status::Ok) { print_status(io, "cannot outline", start, st); return 1; }
    if (inf.kind != nebula::Kind::Dir) { print_status(io, "cannot outline", start, nebula::Status::NotADirectory); return 1; }
    TreeState* t = static_cast<TreeState*>(kheap::alloc_zeroed(sizeof(TreeState)));
    char* path = static_cast<char*>(kheap::alloc(nebula::path::TEXT_MAX));
    if (!t || !path) { if (t) kheap::free(t); if (path) kheap::free(path); print_error(io, "out of memory"); return 1; }
    u32 n = length(start);
    if (n >= nebula::path::TEXT_MAX) { kheap::free(t); kheap::free(path); print_error(io, "path is too long"); return 1; }
    mem::copy(path, start, n + 1);
    t->path = path;
    t->path_len = n;
    t->max_depth = static_cast<u32>(depth);
    t->dirs_only = dirs_only;
    io.out.rgb(palette::ICE);
    io.out.str(start);
    io.out.reset();
    io.out.put('\n');
    tree_walk(sh, io, *t, 0);
    io.out.rgb(palette::MIST);
    io.out.num(t->dirs);
    io.out.str(t->dirs == 1 ? " directory" : " directories");
    if (!dirs_only) {
        io.out.str(", ");
        io.out.num(t->files);
        io.out.str(t->files == 1 ? " file" : " files");
    }
    io.out.reset();
    io.out.put('\n');
    int status = t->stop ? 130 : 0;
    kheap::free(t);
    kheap::free(path);
    return status;
}

struct BeaconCtx {
    Shell* sh;
    Io* io;
    bool numbered;
    u64 line;
    bool at_start;
};

void beacon_emit(const char*, const u8* data, u64 size, void* raw) {
    BeaconCtx& c = *static_cast<BeaconCtx*>(raw);
    Writer& w = c.io->out;
    if (!c.numbered) {
        if (w.terminal()) write_safe(w, data, size);
        else w.bytes(reinterpret_cast<const char*>(data), size);
        return;
    }
    u64 i = 0;
    while (i < size) {
        if (c.sh->poll()) return;
        if (c.at_start) {
            w.rgb(palette::MIST);
            w.pad_num(++c.line, 6);
            w.str("  ");
            w.reset();
            c.at_start = false;
        }
        u64 j = i;
        while (j < size && data[j] != '\n') ++j;
        if (w.terminal()) write_safe(w, data + i, j - i);
        else w.bytes(reinterpret_cast<const char*>(data) + i, j - i);
        if (j < size) { w.put('\n'); c.at_start = true; ++j; }
        i = j;
    }
}

int cmd_beacon(Shell& sh, Io& io, u32 argc, char** argv) {
    BeaconCtx ctx{&sh, &io, false, 0, true};
    if (!opt_bool(io, "numbered", &ctx.numbered)) return 2;
    int st = for_each_input(sh, io, argc, argv, 1, &beacon_emit, &ctx);
    return sh.aborted ? 130 : st;
}

int cmd_probe(Shell& sh, Io& io, u32 argc, char** argv) {
    if (argc < 2) return usage(io, "usage: probe path...");
    rows::Table& out = *io.out_rows;
    rows::begin(out);
    rows::add_column(out, "path", Kind::Text);
    rows::add_column(out, "kind", Kind::Text);
    rows::add_column(out, "size", Kind::Size);
    rows::add_column(out, "changed", Kind::Time);
    rows::add_column(out, "realm", Kind::Text);
    rows::add_column(out, "access", Kind::Text);
    rows::add_column(out, "links", Kind::Int);
    int status = 0;
    for (u32 i = 1; i < argc; ++i) {
        lens::text::StatRow r{};
        nebula::Status st = lens::text::stat_row(sh.cursor, argv[i], &r);
        if (st != nebula::Status::Ok) { print_status(io, "cannot probe", argv[i], st); status = 1; continue; }
        const nebula::Info& inf = r.info;
        char loc[nebula::path::TEXT_MAX];
        nebula::path::to_text(r.path, loc, sizeof(loc));
        u32 row = rows::add_row(out);
        if (row == ~0u) break;
        rows::set_text(out, row, 0, loc);
        rows::set_text(out, row, 1, inf.kind == nebula::Kind::Dir ? "dir" : "file");
        if (inf.kind == nebula::Kind::Dir) rows::set_null(out, row, 2);
        else rows::set_num(out, row, 2, inf.size);
        rows::set_num(out, row, 3, inf.mtime_ms);
        rows::set_text(out, row, 4, r.realm);
        rows::set_text(out, row, 5, r.writable && !inf.read_only ? "writable" : "read-only");
        rows::set_num(out, row, 6, inf.links);
    }
    if (too_many(io, out)) return 1;
    return status;
}

int cmd_spark(Shell& sh, Io& io, u32 argc, char** argv) {
    if (argc < 2) return usage(io, "usage: spark file...");
    int status = 0;
    for (u32 i = 1; i < argc; ++i) {
        nebula::Info inf;
        nebula::Status st = lens::text::stat(sh.cursor, argv[i], &inf);
        if (st == nebula::Status::Ok) continue;
        if (st != nebula::Status::NotFound) { print_status(io, "cannot spark", argv[i], st); status = 1; continue; }
        st = lens::text::write_file(sh.cursor, argv[i], nullptr, 0);
        if (st != nebula::Status::Ok) { print_status(io, "cannot create", argv[i], st); status = 1; }
    }
    return status;
}

int cmd_plant(Shell& sh, Io& io, u32 argc, char** argv) {
    bool parents = false;
    if (!opt_bool(io, "parents", &parents)) return 2;
    if (argc < 2) return usage(io, "usage: plant dir...  parents: yes");
    int status = 0;
    for (u32 i = 1; i < argc; ++i) {
        nebula::Status st = parents ? lens::text::make_path(sh.cursor, argv[i]) : lens::text::make_dir(sh.cursor, argv[i]);
        if (st != nebula::Status::Ok) { print_status(io, "cannot create", argv[i], st); status = 1; }
    }
    return status;
}

int cmd_fade(Shell& sh, Io& io, u32 argc, char** argv) {
    bool tree = false, missing = false;
    if (!opt_bool(io, "tree", &tree) || !opt_bool(io, "missing", &missing)) return 2;
    if (argc < 2) return usage(io, "usage: fade path...  tree: yes  missing: yes");
    int status = 0;
    for (u32 i = 1; i < argc; ++i) {
        if (sh.poll()) return 130;
        nebula::Info inf;
        nebula::Status st = lens::text::stat(sh.cursor, argv[i], &inf);
        if (st == nebula::Status::NotFound && missing) continue;
        if (st != nebula::Status::Ok) { print_status(io, "cannot fade", argv[i], st); status = 1; continue; }
        st = tree ? lens::text::remove_tree(sh.cursor, argv[i], nullptr) : lens::text::remove(sh.cursor, argv[i]);
        if (st == nebula::Status::NotEmpty && !tree) {
            io.err.rgb(palette::SAND);
            io.err.str("  ");
            io.err.str(argv[i]);
            io.err.str(" is not empty; add tree: yes to fade it with everything inside\n");
            io.err.reset();
            status = 1;
            continue;
        }
        if (st != nebula::Status::Ok) { print_status(io, "cannot fade", argv[i], st); status = 1; }
    }
    return status;
}

bool final_target(Shell& sh, const char* src, const char* dst, char* out, u32 cap, bool* exists, nebula::Status* why) {
    nebula::Info di;
    nebula::Status st = lens::text::stat(sh.cursor, dst, &di);
    u32 dl = length(dst);
    if (dl >= cap) { *why = nebula::Status::InvalidPath; return false; }
    mem::copy(out, dst, dl + 1);
    *exists = false;
    if (st == nebula::Status::Ok) {
        *exists = true;
        if (di.kind == nebula::Kind::Dir) {
            nebula::path::Path sp;
            st = lens::text::canonical(sh.cursor, src, &sp);
            if (st != nebula::Status::Ok) { *why = st; return false; }
            const char* leaf = sp.count ? sp.comps[sp.count - 1] : "";
            u32 ln = length(leaf);
            if (ln == 0) { *why = nebula::Status::InvalidPath; return false; }
            u32 p = dl;
            if (p == 0 || out[p - 1] != '/') {
                if (p + 1 >= cap) { *why = nebula::Status::InvalidPath; return false; }
                out[p++] = '/';
            }
            if (p + ln + 1 > cap) { *why = nebula::Status::InvalidPath; return false; }
            mem::copy(out + p, leaf, ln + 1);
            nebula::Info ti;
            *exists = lens::text::stat(sh.cursor, out, &ti) == nebula::Status::Ok;
        }
    } else if (st != nebula::Status::NotFound) {
        *why = st;
        return false;
    }
    return true;
}

void refuse_existing(Io& io, const char* target) {
    io.err.rgb(palette::SAND);
    io.err.str("  ");
    io.err.str(target);
    io.err.str(" already exists; add replace: yes to overwrite it\n");
    io.err.reset();
}

int transfer(Shell& sh, Io& io, u32 argc, char** argv, bool move) {
    bool tree = false, replace = false;
    if (!move && !opt_bool(io, "tree", &tree)) return 2;
    if (!opt_bool(io, "replace", &replace)) return 2;
    const char* use = move ? "usage: drift source... destination  replace: yes" : "usage: twin source... destination  tree: yes  replace: yes";
    if (argc < 3) return usage(io, use);
    const char* dst = argv[argc - 1];
    u32 nsrc = argc - 2;
    nebula::Info di;
    bool dst_is_dir = lens::text::stat(sh.cursor, dst, &di) == nebula::Status::Ok && di.kind == nebula::Kind::Dir;
    if (nsrc > 1 && !dst_is_dir) { print_error(io, "with several sources the destination must be an existing directory"); return 1; }
    const char* verb = move ? "cannot drift" : "cannot twin";
    int status = 0;
    for (u32 i = 0; i < nsrc; ++i) {
        if (sh.poll()) return 130;
        const char* src = argv[1 + i];
        nebula::Info si;
        nebula::Status st = lens::text::stat(sh.cursor, src, &si);
        if (st != nebula::Status::Ok) { print_status(io, verb, src, st); status = 1; continue; }
        char target[nebula::path::TEXT_MAX];
        bool exists = false;
        if (!final_target(sh, src, dst, target, sizeof(target), &exists, &st)) {
            print_status(io, verb, src, st);
            status = 1;
            continue;
        }
        if (exists) {
            nebula::Info ti;
            bool target_dir = lens::text::stat(sh.cursor, target, &ti) == nebula::Status::Ok && ti.kind == nebula::Kind::Dir;
            if (!replace || target_dir || si.kind == nebula::Kind::Dir) {
                if (target_dir || si.kind == nebula::Kind::Dir) {
                    io.err.rgb(palette::SAND);
                    io.err.str("  ");
                    io.err.str(target);
                    io.err.str(" already exists; directories are never merged or replaced, fade it first\n");
                    io.err.reset();
                } else {
                    refuse_existing(io, target);
                }
                status = 1;
                continue;
            }
        }
        if (move) {
            st = lens::text::rename(sh.cursor, src, target, replace ? nebula::RENAME_REPLACE : 0);
            if (st == nebula::Status::CrossRealm) {
                u32 copied = 0;
                st = lens::text::copy_tree(sh.cursor, src, target, &copied);
                if (st == nebula::Status::Ok) st = lens::text::remove_tree(sh.cursor, src, nullptr);
            }
        } else if (si.kind == nebula::Kind::Dir) {
            if (!tree) {
                io.err.rgb(palette::SAND);
                io.err.str("  ");
                io.err.str(src);
                io.err.str(" is a directory; add tree: yes to twin it with everything inside\n");
                io.err.reset();
                status = 1;
                continue;
            }
            u32 copied = 0;
            st = lens::text::copy_tree(sh.cursor, src, target, &copied);
        } else {
            st = lens::text::copy_file(sh.cursor, src, target);
        }
        if (st != nebula::Status::Ok) { print_status(io, verb, src, st); status = 1; }
    }
    return status;
}

int cmd_twin(Shell& sh, Io& io, u32 argc, char** argv) { return transfer(sh, io, argc, argv, false); }
int cmd_drift(Shell& sh, Io& io, u32 argc, char** argv) { return transfer(sh, io, argc, argv, true); }

int cmd_etch(Shell& sh, Io& io, u32 argc, char** argv) {
    bool append = false;
    if (!opt_bool(io, "append", &append)) return 2;
    if (argc != 2) return usage(io, "usage: etch file  append: yes   (writes what is piped in)");
    if (!io.has_in) { print_error(io, "nothing to write: pipe something in, for example  chart | etch listing.txt"); return 1; }
    nebula::Status st = append ? lens::text::append_file(sh.cursor, argv[1], io.in, io.in_size)
                               : lens::text::write_file(sh.cursor, argv[1], io.in, io.in_size);
    if (st != nebula::Status::Ok) { print_status(io, "cannot write", argv[1], st); return 1; }
    return 0;
}

int cmd_realms(Shell&, Io& io, u32 argc, char**) {
    if (argc != 1) return usage(io, "usage: realms");
    rows::Table& out = *io.out_rows;
    rows::begin(out);
    rows::add_column(out, "realm", Kind::Text);
    rows::add_column(out, "state", Kind::Text);
    rows::add_column(out, "access", Kind::Text);
    rows::add_column(out, "free", Kind::Size);
    rows::add_column(out, "size", Kind::Size);
    rows::add_column(out, "about", Kind::Text);
    for (u32 i = 0; i < nebula::realm_count(); ++i) {
        nebula::Realm* r = nebula::realm_at(i);
        u32 row = rows::add_row(out);
        if (row == ~0u) break;
        rows::set_text(out, row, 0, r->name());
        rows::set_text(out, row, 1, r->available() ? "available" : "unavailable");
        rows::set_text(out, row, 2, r->writable() ? "writable" : "read-only");
        u64 total = r->space_total();
        if (r->available() && total) {
            rows::set_num(out, row, 3, r->space_free());
            rows::set_num(out, row, 4, total);
        } else {
            rows::set_null(out, row, 3);
            rows::set_null(out, row, 4);
        }
        rows::set_text(out, row, 5, r->summary());
    }
    return too_many(io, out) ? 1 : 0;
}

int cmd_epoch(Shell&, Io& io, u32 argc, char** argv) {
    if (!nebula::disk_is_mounted()) { print_error(io, "no disk volume is mounted"); return 1; }
    if (argc == 2 && same(argv[1], "take")) {
        u64 id = 0;
        nebula::Status st = nebula::disk_snapshot(&id);
        if (st != nebula::Status::Ok) { print_status(io, "cannot take an epoch of", "disk", st); return 1; }
        io.out.str("  epoch ");
        io.out.num(id);
        io.out.str(" taken; browse it at ");
        io.out.rgb(palette::ICE);
        io.out.str("snap:/");
        io.out.num(id);
        io.out.reset();
        io.out.put('\n');
        return 0;
    }
    if (argc == 1 || (argc == 2 && same(argv[1], "list"))) {
        u64 ids[stellar::SNAPSHOT_MAX];
        u32 n = stellar::snapshot_ids(ids, stellar::SNAPSHOT_MAX);
        rows::Table& out = *io.out_rows;
        rows::begin(out);
        rows::add_column(out, "epoch", Kind::Int);
        rows::add_column(out, "path", Kind::Text);
        for (u32 i = 0; i < n; ++i) {
            u32 row = rows::add_row(out);
            if (row == ~0u) break;
            rows::set_num(out, row, 0, ids[i]);
            char path[32] = "snap:/";
            u32 p = 6;
            char digits[24];
            u32 d = 0;
            u64 v = ids[i];
            if (!v) digits[d++] = '0';
            while (v) { digits[d++] = static_cast<char>('0' + v % 10); v /= 10; }
            while (d) path[p++] = digits[--d];
            path[p] = 0;
            rows::set_text(out, row, 1, path);
        }
        char note[48] = "of ";
        u32 p = 3;
        char digits[24];
        u32 d = 0;
        u64 v = stellar::SNAPSHOT_MAX;
        while (v) { digits[d++] = static_cast<char>('0' + v % 10); v /= 10; }
        while (d) note[p++] = digits[--d];
        const char* tail = " epochs possible";
        for (u32 i = 0; tail[i]; ++i) note[p++] = tail[i];
        note[p] = 0;
        rows::set_note(out, note);
        return too_many(io, out) ? 1 : 0;
    }
    if (argc == 3 && same(argv[1], "drop")) {
        u64 id;
        if (!parse_u64(argv[2], &id)) return usage(io, "usage: epoch drop ID");
        nebula::Status st = nebula::disk_snapshot_delete(id);
        if (st != nebula::Status::Ok) { print_status(io, "cannot drop epoch", argv[2], st); return 1; }
        return 0;
    }
    return usage(io, "usage: epoch [list|take|drop ID]");
}

int cmd_audit(Shell&, Io& io, u32 argc, char**) {
    if (!nebula::disk_is_mounted()) { print_error(io, "no disk volume is mounted"); return 1; }
    if (argc != 1) return usage(io, "usage: audit  deep: yes   (deep verifies every checksum and is slow on large volumes)");
    bool deep = false;
    if (!opt_bool(io, "deep", &deep)) return 2;
    stellar::CheckReport rep{};
    stellar::Status why = stellar::Status::Ok;
    bool ok = stellar::check(&rep, deep, &why);
    Writer& w = io.out;
    auto line = [&](const char* k, u64 v) { w.str("  "); w.pad_to(k, 14); w.num(v); w.put('\n'); };
    line("epochs", rep.snapshots);
    line("nodes", rep.nodes);
    line("directories", rep.dirs);
    line("files", rep.files);
    line("entries", rep.entries);
    line("bad checksums", rep.bad_crc);
    line("bad files", rep.bad_files);
    line("bad links", rep.bad_links);
    line("bad structure", rep.bad_structure);
    line("bad bitmap", rep.bad_bitmap);
    w.str("  ");
    if (ok && rep.ok()) { w.rgb(palette::MINT); w.str("volume is consistent"); }
    else { w.rgb(palette::SAND); w.str("volume has problems"); if (!ok) { w.str(" ("); w.str(stellar::status_name(why)); w.put(')'); } }
    w.reset();
    w.put('\n');
    return ok && rep.ok() ? 0 : 1;
}

int cmd_settle(Shell&, Io& io, u32 argc, char**) {
    if (argc != 1) return usage(io, "usage: settle");
    if (!blockdev::present()) { print_error(io, "no block device"); return 1; }
    if (!blockdev::flush()) { print_error(io, "the device refused the flush"); return 1; }
    return 0;
}

int cmd_kindle(Shell&, Io& io, u32 argc, char** argv) {
    if (argc != 2 || !same(argv[1], "erase"))
        return usage(io, "usage: kindle erase   (creates an empty Stellar volume; only on a blank disk or one that already holds a Stellar volume)");
    if (!blockdev::present()) { print_error(io, "no block device"); return 1; }
    if (nebula::disk_is_confirmed_only()) {
        print_error(io, "this disk needs a typed confirmation; use: storage init erase <MiB>");
        return 1;
    }
    if (nebula::disk_is_mounted()) { print_error(io, "the disk volume is mounted; refusing to erase it"); return 1; }
    if (!stellar::can_auto_format()) {
        print_error(io, "the disk is not blank and holds no Stellar volume; refusing to touch it");
        return 1;
    }
    stellar::Status why = stellar::Status::Ok;
    if (!stellar::format(blockdev::capacity_sectors(), &why)) {
        print_status(io, "cannot format", "disk", nebula::status_from_stellar(why));
        return 1;
    }
    nebula::disk_set_mounted(true);
    io.out.str("  ");
    io.out.rgb(palette::MINT);
    io.out.str("new Stellar volume created and mounted as disk:");
    io.out.reset();
    io.out.put('\n');
    return 0;
}

constexpr Command COMMANDS[] = {
    {"here", "here", "show the current location", &cmd_here},
    {"go", "go [path]", "change location; realm: switches realm; no path goes to the realm root", &cmd_go},
    {"back", "back", "return to the previous location", &cmd_back},
    {"chart", "chart [path]  depth: N  only: dir|file", "list a directory as rows: name, kind, size, changed", &cmd_chart, "depth only"},
    {"outline", "outline [path]  depth: N  only: dir", "draw a directory as a tree", &cmd_outline, "depth only"},
    {"beacon", "beacon [file...]  numbered: yes", "show files or piped input", &cmd_beacon, "numbered"},
    {"probe", "probe path...", "kind, size, time, realm and access of paths, as rows", &cmd_probe},
    {"spark", "spark file...", "create empty files that do not exist", &cmd_spark},
    {"plant", "plant dir...  parents: yes", "create directories", &cmd_plant, "parents"},
    {"fade", "fade path...  tree: yes  missing: yes", "remove files and empty directories; tree: yes takes everything inside too", &cmd_fade, "tree missing"},
    {"twin", "twin source... dest  tree: yes  replace: yes", "copy files, and with tree: yes directories; works across realms", &cmd_twin, "tree replace"},
    {"drift", "drift source... dest  replace: yes", "move or rename; across realms it copies then removes", &cmd_drift, "replace"},
    {"etch", "etch file  append: yes", "write what is piped in to a file", &cmd_etch, "append"},
    {"realms", "realms", "the namespaces ram, disk, snap, sys and their space, as rows", &cmd_realms},
    {"epoch", "epoch [list|take|drop ID]", "freeze the disk volume as an epoch; browse it under snap:/", &cmd_epoch},
    {"audit", "audit  deep: yes", "verify the Stellar volume's structures", &cmd_audit, "deep"},
    {"settle", "settle", "ask the disk to flush its write cache", &cmd_settle},
    {"kindle", "kindle erase", "create an empty Stellar volume on a blank disk", &cmd_kindle},
};

constexpr Table TABLE{"files and realms", COMMANDS, sizeof(COMMANDS) / sizeof(COMMANDS[0])};

}

void register_fs_commands() { register_table(&TABLE); }

}
