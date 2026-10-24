#include <cosmos/helm.hpp>
#include <cosmos/nebula_realms.hpp>
#include <cosmos/blockdev.hpp>
#include <cosmos/kheap.hpp>

namespace helm {

namespace {

bool same(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

u32 length(const char* s) { return static_cast<u32>(cstr::length(s)); }

int usage(Io& io, const char* text) {
    print_error(io, text);
    return 2;
}

int cmd_pwd(Shell& sh, Io& io, u32, char**) {
    char loc[nebula::path::TEXT_MAX];
    lens::text::location_text(sh.cursor, loc, sizeof(loc));
    io.out.str(loc);
    io.out.put('\n');
    return 0;
}

int cmd_cd(Shell& sh, Io& io, u32 argc, char** argv) {
    if (argc > 2) return usage(io, "usage: cd [path|-]");
    char target[nebula::path::TEXT_MAX];
    nebula::path::Path before = sh.cursor.here;
    if (argc == 1) {
        u32 n = length(sh.cursor.here.realm);
        mem::copy(target, sh.cursor.here.realm, n);
        target[n] = ':';
        target[n + 1] = 0;
    } else if (same(argv[1], "-")) {
        if (!sh.has_previous) { print_error(io, "no previous location"); return 1; }
        nebula::path::to_text(sh.previous, target, sizeof(target));
    } else {
        u32 n = length(argv[1]);
        if (n >= sizeof(target)) { print_error(io, "path is too long"); return 1; }
        mem::copy(target, argv[1], n + 1);
    }
    nebula::Status st = lens::text::change_location(sh.cursor, target);
    if (st != nebula::Status::Ok) { print_status(io, "cannot enter", target, st); return 1; }
    sh.previous = before;
    sh.has_previous = true;
    return 0;
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

void print_long_row(Writer& w, nebula::Kind kind, u64 size, u64 mtime, const char* name) {
    w.rgb(palette::MIST);
    w.put(kind == nebula::Kind::Dir ? 'd' : '-');
    w.put(' ');
    if (kind == nebula::Kind::Dir) {
        w.spaces(9);
        w.put('-');
    } else {
        w.pad_num(size, 10);
    }
    w.str("  ");
    char t[24];
    format_time(mtime, t);
    w.str(t);
    w.str("  ");
    w.reset();
    print_entry_name(w, name, kind);
    w.put('\n');
}

int ls_path(Shell& sh, Io& io, const char* path, bool lng, bool one, bool header) {
    nebula::Info inf;
    nebula::Status st = lens::text::stat(sh.cursor, path, &inf);
    if (st != nebula::Status::Ok) { print_status(io, "cannot list", path, st); return 1; }
    if (inf.kind == nebula::Kind::File) {
        if (lng) print_long_row(io.out, inf.kind, inf.size, inf.mtime_ms, path);
        else { io.out.str(path); io.out.put('\n'); }
        return 0;
    }
    nebula::Listing l;
    st = lens::text::list(sh.cursor, path, &l, lng);
    if (st != nebula::Status::Ok) { print_status(io, "cannot list", path, st); return 1; }
    if (header) {
        io.out.rgb(palette::AMBER);
        io.out.str(path);
        io.out.str(":");
        io.out.reset();
        io.out.put('\n');
    }
    if (lng) {
        for (u32 i = 0; i < l.count; ++i) {
            if (sh.poll()) break;
            print_long_row(io.out, l.items[i].kind, l.items[i].size, l.items[i].mtime_ms, l.items[i].name);
        }
    } else if (one || !io.out.terminal()) {
        for (u32 i = 0; i < l.count; ++i) {
            if (sh.poll()) break;
            print_entry_name(io.out, l.items[i].name, l.items[i].kind);
            io.out.put('\n');
        }
    } else if (l.count) {
        u32 widest = 0;
        for (u32 i = 0; i < l.count; ++i) {
            u32 c = utf8::cells(l.items[i].name, length(l.items[i].name)) + (l.items[i].kind == nebula::Kind::Dir ? 1 : 0);
            if (c > widest) widest = c;
        }
        u32 col_w = widest + 2;
        u32 cols = sh.cols / col_w;
        if (cols == 0) cols = 1;
        u32 rows = (l.count + cols - 1) / cols;
        for (u32 r = 0; r < rows; ++r) {
            for (u32 c = 0; c < cols; ++c) {
                u32 idx = c * rows + r;
                if (idx >= l.count) break;
                const nebula::Entry& e = l.items[idx];
                print_entry_name(io.out, e.name, e.kind);
                bool more = (c + 1) * rows + r < l.count;
                if (more) {
                    u32 used = utf8::cells(e.name, length(e.name)) + (e.kind == nebula::Kind::Dir ? 1 : 0);
                    io.out.spaces(col_w - used);
                }
            }
            io.out.put('\n');
        }
    }
    if (l.truncated) print_error(io, "listing was cut off at 65536 entries");
    nebula::release(l);
    return 0;
}

int cmd_ls(Shell& sh, Io& io, u32 argc, char** argv) {
    Options o = options_begin(argc, argv);
    bool lng = false, one = false;
    int c;
    while ((c = next_option(o, "la1", "")) >= 0) {
        if (c == 'l') lng = true;
        else if (c == '1') one = true;
    }
    if (o.bad) return usage(io, "usage: ls [-l] [-1] [path...]");
    if (o.index >= argc) return ls_path(sh, io, ".", lng, one, false);
    int status = 0;
    bool header = argc - o.index > 1;
    for (u32 i = o.index; i < argc; ++i) {
        if (i > o.index) io.out.put('\n');
        if (ls_path(sh, io, argv[i], lng, one, header) != 0) status = 1;
        if (sh.aborted) return 130;
    }
    return status;
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
        io.err.rgb(palette::SAND);
        io.err.str("  cannot list ");
        io.err.str(t.path);
        io.err.str(": ");
        io.err.str(nebula::status_text(st));
        io.err.str("\n");
        io.err.reset();
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

int cmd_tree(Shell& sh, Io& io, u32 argc, char** argv) {
    Options o = options_begin(argc, argv);
    u32 depth = 6;
    bool dirs_only = false;
    int c;
    while ((c = next_option(o, "d", "L")) >= 0) {
        if (c == 'd') dirs_only = true;
        else if (c == 'L') {
            u64 v;
            if (!parse_u64(o.value, &v) || v == 0 || v > 15) return usage(io, "tree: -L takes a depth from 1 to 15");
            depth = static_cast<u32>(v);
        }
    }
    if (o.bad) return usage(io, "usage: tree [-d] [-L depth] [path]");
    if (argc - o.index > 1) return usage(io, "usage: tree [-d] [-L depth] [path]");
    const char* start = o.index < argc ? argv[o.index] : ".";
    nebula::Info inf;
    nebula::Status st = lens::text::stat(sh.cursor, start, &inf);
    if (st != nebula::Status::Ok) { print_status(io, "cannot list", start, st); return 1; }
    if (inf.kind != nebula::Kind::Dir) { print_status(io, "cannot list", start, nebula::Status::NotADirectory); return 1; }
    TreeState* t = static_cast<TreeState*>(kheap::alloc_zeroed(sizeof(TreeState)));
    char* path = static_cast<char*>(kheap::alloc(nebula::path::TEXT_MAX));
    if (!t || !path) { if (t) kheap::free(t); if (path) kheap::free(path); print_error(io, "out of memory"); return 1; }
    u32 n = length(start);
    if (n >= nebula::path::TEXT_MAX) { kheap::free(t); kheap::free(path); print_error(io, "path is too long"); return 1; }
    mem::copy(path, start, n + 1);
    t->path = path;
    t->path_len = n;
    t->max_depth = depth;
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

void write_sanitized(Writer& w, const u8* data, u64 size) {
    utf8::Decoder d;
    u32 cps[2];
    for (u64 i = 0; i < size; ++i) {
        u8 b = data[i];
        u32 k = d.feed(b, cps);
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

struct CatCtx {
    Shell* sh;
    Io* io;
    bool numbered;
    u64 line;
    bool at_start;
};

void cat_emit(const char*, const u8* data, u64 size, void* raw) {
    CatCtx& c = *static_cast<CatCtx*>(raw);
    Writer& w = c.io->out;
    if (!c.numbered) {
        if (w.terminal()) write_sanitized(w, data, size);
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
        if (w.terminal()) write_sanitized(w, data + i, j - i);
        else w.bytes(reinterpret_cast<const char*>(data) + i, j - i);
        if (j < size) { w.put('\n'); c.at_start = true; ++j; }
        i = j;
    }
}

int cmd_cat(Shell& sh, Io& io, u32 argc, char** argv) {
    Options o = options_begin(argc, argv);
    CatCtx ctx{&sh, &io, false, 0, true};
    int c;
    while ((c = next_option(o, "n", "")) >= 0) ctx.numbered = true;
    if (o.bad) return usage(io, "usage: cat [-n] [file...]");
    int st = for_each_input(sh, io, o, &cat_emit, &ctx);
    return sh.aborted ? 130 : st;
}

int cmd_stat(Shell& sh, Io& io, u32 argc, char** argv) {
    if (argc < 2) return usage(io, "usage: stat path...");
    int status = 0;
    for (u32 i = 1; i < argc; ++i) {
        lens::text::StatRow r{};
        nebula::Status st = lens::text::stat_row(sh.cursor, argv[i], &r);
        const nebula::Info& inf = r.info;
        if (st != nebula::Status::Ok) { print_status(io, "cannot stat", argv[i], st); status = 1; continue; }
        char loc[nebula::path::TEXT_MAX];
        nebula::path::to_text(r.path, loc, sizeof(loc));
        auto row = [&](const char* k) { io.out.rgb(palette::AMBER); io.out.pad_to(k, 9); io.out.reset(); };
        row("path"); io.out.str(loc); io.out.put('\n');
        row("kind"); io.out.str(inf.kind == nebula::Kind::Dir ? "directory" : "file"); io.out.put('\n');
        if (inf.kind == nebula::Kind::File) {
            row("size");
            io.out.num(inf.size);
            io.out.str(" bytes");
            if (inf.size >= 1024) { io.out.str(" ("); print_size(io.out, inf.size); io.out.put(')'); }
            io.out.put('\n');
        }
        char t[24];
        format_time(inf.mtime_ms, t);
        row("changed"); io.out.str(t); io.out.put('\n');
        row("realm"); io.out.str(r.realm); io.out.str(r.writable && !inf.read_only ? ", writable" : ", read-only"); io.out.put('\n');
        row("links"); io.out.num(inf.links); io.out.put('\n');
    }
    return status;
}

int cmd_touch(Shell& sh, Io& io, u32 argc, char** argv) {
    if (argc < 2) return usage(io, "usage: touch file...");
    int status = 0;
    for (u32 i = 1; i < argc; ++i) {
        nebula::Info inf;
        nebula::Status st = lens::text::stat(sh.cursor, argv[i], &inf);
        if (st == nebula::Status::Ok) continue;
        if (st != nebula::Status::NotFound) { print_status(io, "cannot touch", argv[i], st); status = 1; continue; }
        st = lens::text::write_file(sh.cursor, argv[i], nullptr, 0);
        if (st != nebula::Status::Ok) { print_status(io, "cannot create", argv[i], st); status = 1; }
    }
    return status;
}

int cmd_mkdir(Shell& sh, Io& io, u32 argc, char** argv) {
    Options o = options_begin(argc, argv);
    bool parents = false;
    int c;
    while ((c = next_option(o, "p", "")) >= 0) parents = true;
    if (o.bad || o.index >= argc) return usage(io, "usage: mkdir [-p] dir...");
    int status = 0;
    for (u32 i = o.index; i < argc; ++i) {
        nebula::Status st = parents ? lens::text::make_path(sh.cursor, argv[i]) : lens::text::make_dir(sh.cursor, argv[i]);
        if (st != nebula::Status::Ok) { print_status(io, "cannot create", argv[i], st); status = 1; }
    }
    return status;
}

int cmd_rm(Shell& sh, Io& io, u32 argc, char** argv) {
    Options o = options_begin(argc, argv);
    bool recursive = false, force = false;
    int c;
    while ((c = next_option(o, "rf", "")) >= 0) {
        if (c == 'r') recursive = true;
        else force = true;
    }
    if (o.bad || o.index >= argc) return usage(io, "usage: rm [-r] [-f] path...");
    int status = 0;
    for (u32 i = o.index; i < argc; ++i) {
        if (sh.poll()) return 130;
        nebula::Info inf;
        nebula::Status st = lens::text::stat(sh.cursor, argv[i], &inf);
        if (st == nebula::Status::NotFound && force) continue;
        if (st != nebula::Status::Ok) { print_status(io, "cannot remove", argv[i], st); status = 1; continue; }
        if (inf.kind == nebula::Kind::Dir && !recursive) {
            io.err.rgb(palette::SAND);
            io.err.str("  ");
            io.err.str(argv[i]);
            io.err.str(" is a directory: use rm -r, or rmdir if it is empty\n");
            io.err.reset();
            status = 1;
            continue;
        }
        st = recursive ? lens::text::remove_tree(sh.cursor, argv[i], nullptr) : lens::text::remove(sh.cursor, argv[i]);
        if (st != nebula::Status::Ok) { print_status(io, "cannot remove", argv[i], st); status = 1; }
    }
    return status;
}

int cmd_rmdir(Shell& sh, Io& io, u32 argc, char** argv) {
    if (argc < 2) return usage(io, "usage: rmdir dir...");
    int status = 0;
    for (u32 i = 1; i < argc; ++i) {
        nebula::Info inf;
        nebula::Status st = lens::text::stat(sh.cursor, argv[i], &inf);
        if (st == nebula::Status::Ok && inf.kind != nebula::Kind::Dir) st = nebula::Status::NotADirectory;
        if (st == nebula::Status::Ok) st = lens::text::remove(sh.cursor, argv[i]);
        if (st != nebula::Status::Ok) { print_status(io, "cannot remove", argv[i], st); status = 1; }
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
    io.err.str(" already exists; use -f to replace it\n");
    io.err.reset();
}

int transfer(Shell& sh, Io& io, u32 argc, char** argv, bool move) {
    Options o = options_begin(argc, argv);
    bool recursive = false, force = false;
    int c;
    while ((c = next_option(o, move ? "f" : "rf", "")) >= 0) {
        if (c == 'r') recursive = true;
        else force = true;
    }
    const char* use = move ? "usage: mv [-f] source... destination" : "usage: cp [-r] [-f] source... destination";
    if (o.bad || argc - o.index < 2) return usage(io, use);
    const char* dst = argv[argc - 1];
    u32 nsrc = argc - 1 - o.index;
    nebula::Info di;
    bool dst_is_dir = lens::text::stat(sh.cursor, dst, &di) == nebula::Status::Ok && di.kind == nebula::Kind::Dir;
    if (nsrc > 1 && !dst_is_dir) { print_error(io, "with several sources the destination must be an existing directory"); return 1; }
    int status = 0;
    for (u32 i = 0; i < nsrc; ++i) {
        if (sh.poll()) return 130;
        const char* src = argv[o.index + i];
        nebula::Info si;
        nebula::Status st = lens::text::stat(sh.cursor, src, &si);
        if (st != nebula::Status::Ok) { print_status(io, move ? "cannot move" : "cannot copy", src, st); status = 1; continue; }
        char target[nebula::path::TEXT_MAX];
        bool exists = false;
        if (!final_target(sh, src, dst, target, sizeof(target), &exists, &st)) {
            print_status(io, move ? "cannot move" : "cannot copy", src, st);
            status = 1;
            continue;
        }
        if (exists) {
            nebula::Info ti;
            bool target_dir = lens::text::stat(sh.cursor, target, &ti) == nebula::Status::Ok && ti.kind == nebula::Kind::Dir;
            if (!force || target_dir || si.kind == nebula::Kind::Dir) {
                if (target_dir || si.kind == nebula::Kind::Dir) {
                    io.err.rgb(palette::SAND);
                    io.err.str("  ");
                    io.err.str(target);
                    io.err.str(" already exists; directories are never merged or replaced, remove it first\n");
                    io.err.reset();
                } else {
                    refuse_existing(io, target);
                }
                status = 1;
                continue;
            }
        }
        if (move) {
            st = lens::text::rename(sh.cursor, src, target, force ? nebula::RENAME_REPLACE : 0);
            if (st == nebula::Status::CrossRealm) {
                u32 copied = 0;
                st = lens::text::copy_tree(sh.cursor, src, target, &copied);
                if (st == nebula::Status::Ok) st = lens::text::remove_tree(sh.cursor, src, nullptr);
            }
        } else if (si.kind == nebula::Kind::Dir) {
            if (!recursive) {
                io.err.rgb(palette::SAND);
                io.err.str("  ");
                io.err.str(src);
                io.err.str(" is a directory: use cp -r\n");
                io.err.reset();
                status = 1;
                continue;
            }
            u32 copied = 0;
            st = lens::text::copy_tree(sh.cursor, src, target, &copied);
        } else {
            st = lens::text::copy_file(sh.cursor, src, target);
        }
        if (st != nebula::Status::Ok) { print_status(io, move ? "cannot move" : "cannot copy", src, st); status = 1; }
    }
    return status;
}

int cmd_cp(Shell& sh, Io& io, u32 argc, char** argv) { return transfer(sh, io, argc, argv, false); }
int cmd_mv(Shell& sh, Io& io, u32 argc, char** argv) { return transfer(sh, io, argc, argv, true); }

int cmd_realms(Shell&, Io& io, u32, char**) {
    Writer& w = io.out;
    w.rgb(palette::MIST);
    w.str("  realm   state        mode       size\n");
    w.reset();
    for (u32 i = 0; i < nebula::realm_count(); ++i) {
        nebula::Realm* r = nebula::realm_at(i);
        w.str("  ");
        w.rgb(palette::ICE);
        w.pad_to(r->name(), 8);
        w.reset();
        w.pad_to(r->available() ? "available" : "unavailable", 13);
        w.pad_to(r->writable() ? "writable" : "read-only", 11);
        u64 total = r->space_total();
        if (r->available() && total) {
            print_size(w, r->space_free());
            w.str(" free of ");
            print_size(w, total);
        } else {
            w.str("-");
        }
        w.put('\n');
        w.str("          ");
        w.rgb(palette::MIST);
        w.str(r->summary());
        w.reset();
        w.put('\n');
    }
    return 0;
}

struct FindCtx {
    Shell* sh;
    Io* io;
    char* path;
    u32 path_len;
    char pattern[64];
    bool has_pattern;
    int type;
    u64 hits;
    bool stop;
};

void find_walk(FindCtx& f, u32 depth) {
    if (depth > nebula::MAX_TREE_DEPTH || f.stop) return;
    nebula::Listing l;
    f.path[f.path_len] = 0;
    if (lens::text::list(f.sh->cursor, f.path, &l, false) != nebula::Status::Ok) return;
    for (u32 i = 0; i < l.count && !f.stop; ++i) {
        if (f.sh->poll()) { f.stop = true; break; }
        const nebula::Entry& e = l.items[i];
        bool is_dir = e.kind == nebula::Kind::Dir;
        u32 saved = f.path_len;
        u32 nl = length(e.name);
        if (f.path_len + 1 + nl + 1 >= nebula::path::TEXT_MAX) continue;
        if (f.path_len && f.path[f.path_len - 1] != '/') f.path[f.path_len++] = '/';
        mem::copy(f.path + f.path_len, e.name, nl);
        f.path_len += nl;
        f.path[f.path_len] = 0;
        bool type_ok = f.type == 0 || (f.type == 'd' && is_dir) || (f.type == 'f' && !is_dir);
        if (type_ok && (!f.has_pattern || glob_match(f.pattern, e.name))) {
            f.io->out.str(f.path);
            f.io->out.put('\n');
            ++f.hits;
        }
        if (is_dir) find_walk(f, depth + 1);
        f.path_len = saved;
    }
    nebula::release(l);
}

int cmd_find(Shell& sh, Io& io, u32 argc, char** argv) {
    const char* start = ".";
    FindCtx f{};
    f.sh = &sh;
    f.io = &io;
    bool have_start = false;
    for (u32 i = 1; i < argc; ++i) {
        if (same(argv[i], "-name") && i + 1 < argc) {
            const char* p = argv[++i];
            if (length(p) >= sizeof(f.pattern)) return usage(io, "find: pattern is too long");
            mark_wildcards(p, f.pattern, sizeof(f.pattern));
            f.has_pattern = true;
        } else if (same(argv[i], "-type") && i + 1 < argc) {
            const char* t = argv[++i];
            if (!(same(t, "f") || same(t, "d"))) return usage(io, "find: -type takes f or d");
            f.type = t[0];
        } else if (argv[i][0] != '-' && !have_start) {
            start = argv[i];
            have_start = true;
        } else {
            return usage(io, "usage: find [path] [-name pattern] [-type f|d]   (quote wildcards: -name '*.txt')");
        }
    }
    nebula::Info inf;
    nebula::Status st = lens::text::stat(sh.cursor, start, &inf);
    if (st != nebula::Status::Ok) { print_status(io, "cannot search", start, st); return 1; }
    if (inf.kind != nebula::Kind::Dir) { print_status(io, "cannot search", start, nebula::Status::NotADirectory); return 1; }
    f.path = static_cast<char*>(kheap::alloc(nebula::path::TEXT_MAX));
    if (!f.path) { print_error(io, "out of memory"); return 1; }
    u32 n = length(start);
    if (n >= nebula::path::TEXT_MAX) { kheap::free(f.path); print_error(io, "path is too long"); return 1; }
    mem::copy(f.path, start, n + 1);
    f.path_len = n;
    find_walk(f, 0);
    kheap::free(f.path);
    if (f.stop) return 130;
    return f.hits ? 0 : 1;
}

int cmd_snapshot(Shell&, Io& io, u32 argc, char** argv) {
    if (!nebula::disk_is_mounted()) { print_error(io, "no disk volume is mounted"); return 1; }
    if (argc == 1 || same(argv[1], "take")) {
        u64 id = 0;
        nebula::Status st = nebula::disk_snapshot(&id);
        if (st != nebula::Status::Ok) { print_status(io, "cannot snapshot", "disk", st); return 1; }
        io.out.str("  snapshot ");
        io.out.num(id);
        io.out.str(" taken; browse it at ");
        io.out.rgb(palette::ICE);
        io.out.str("snap:/");
        io.out.num(id);
        io.out.reset();
        io.out.put('\n');
        return 0;
    }
    if (same(argv[1], "list")) {
        u64 ids[stellar::SNAPSHOT_MAX];
        u32 n = stellar::snapshot_ids(ids, stellar::SNAPSHOT_MAX);
        for (u32 i = 0; i < n; ++i) {
            io.out.str("  ");
            io.out.rgb(palette::ICE);
            io.out.str("snap:/");
            io.out.num(ids[i]);
            io.out.reset();
            io.out.put('\n');
        }
        io.out.rgb(palette::MIST);
        io.out.num(n);
        io.out.str(" of ");
        io.out.num(stellar::SNAPSHOT_MAX);
        io.out.str(" snapshots\n");
        io.out.reset();
        return 0;
    }
    if (same(argv[1], "rm") && argc == 3) {
        u64 id;
        if (!parse_u64(argv[2], &id)) return usage(io, "usage: snapshot rm id");
        nebula::Status st = nebula::disk_snapshot_delete(id);
        if (st != nebula::Status::Ok) { print_status(io, "cannot delete snapshot", argv[2], st); return 1; }
        return 0;
    }
    return usage(io, "usage: snapshot [take|list|rm id]");
}

int cmd_check(Shell&, Io& io, u32 argc, char** argv) {
    if (!nebula::disk_is_mounted()) { print_error(io, "no disk volume is mounted"); return 1; }
    bool deep = false;
    if (argc == 2 && same(argv[1], "-d")) deep = true;
    else if (argc != 1) return usage(io, "usage: check [-d]   (-d verifies every checksum and is slow on large volumes)");
    stellar::CheckReport rep{};
    stellar::Status why = stellar::Status::Ok;
    bool ok = stellar::check(&rep, deep, &why);
    Writer& w = io.out;
    auto line = [&](const char* k, u64 v) { w.str("  "); w.pad_to(k, 14); w.num(v); w.put('\n'); };
    line("snapshots", rep.snapshots);
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

int cmd_sync(Shell&, Io& io, u32, char**) {
    if (!blockdev::present()) { print_error(io, "no block device"); return 1; }
    if (!blockdev::flush()) { print_error(io, "the device refused the flush"); return 1; }
    return 0;
}

int cmd_mkfs(Shell&, Io& io, u32 argc, char** argv) {
    if (argc != 2 || !same(argv[1], "erase"))
        return usage(io, "usage: mkfs erase   (creates an empty Stellar volume; only on a blank disk or one that already holds a Stellar volume)");
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
    {"pwd", "pwd", "show the current location", &cmd_pwd},
    {"cd", "cd [path|-]", "change location; realm: switches realm; no argument goes to the realm root", &cmd_cd},
    {"ls", "ls [-l] [-1] [path...]", "list a directory; -l shows size and time", &cmd_ls},
    {"tree", "tree [-d] [-L depth] [path]", "draw a directory tree", &cmd_tree},
    {"cat", "cat [-n] [file...]", "print files or standard input", &cmd_cat},
    {"stat", "stat path...", "kind, size, time and realm of a path", &cmd_stat},
    {"touch", "touch file...", "create empty files that do not exist", &cmd_touch},
    {"mkdir", "mkdir [-p] dir...", "create directories", &cmd_mkdir},
    {"rm", "rm [-r] [-f] path...", "remove files; -r removes directories with their contents", &cmd_rm},
    {"rmdir", "rmdir dir...", "remove empty directories", &cmd_rmdir},
    {"cp", "cp [-r] [-f] source... dest", "copy files and, with -r, directories; works across realms", &cmd_cp},
    {"mv", "mv [-f] source... dest", "move or rename; across realms it copies then removes", &cmd_mv},
    {"find", "find [path] [-name pat] [-type f|d]", "search a tree; quote wildcards", &cmd_find},
    {"realms", "realms", "the namespaces: ram, disk, snap, sys, and their space", &cmd_realms},
    {"df", "df", "same as realms", &cmd_realms},
    {"snapshot", "snapshot [take|list|rm id]", "freeze the disk volume; browse it under snap:/", &cmd_snapshot},
    {"check", "check [-d]", "verify the Stellar volume's structures", &cmd_check},
    {"sync", "sync", "ask the disk to flush its write cache", &cmd_sync},
    {"mkfs", "mkfs erase", "create an empty Stellar volume on a blank disk", &cmd_mkfs},
};

constexpr Table TABLE{"files and realms", COMMANDS, sizeof(COMMANDS) / sizeof(COMMANDS[0])};

}

void register_fs_commands() { register_table(&TABLE); }

}
