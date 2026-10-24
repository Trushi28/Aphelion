#include <cosmos/lens_text.hpp>
#include <cosmos/nebula_realms.hpp>
#include <cosmos/kheap.hpp>

namespace lens {

namespace {

constinit Domain g_boot{};
constinit Lens g_boot_roots[nebula::MAX_REALMS]{};
constinit nebula::Realm* g_boot_realms[nebula::MAX_REALMS]{};
u32 g_boot_count;
constinit Lens g_boot_console{};

}

Domain& boot_domain() { return g_boot; }

Done mint_boot_roots() {
    g_boot_count = 0;
    for (u32 i = 0; i < nebula::realm_count() && i < nebula::MAX_REALMS; ++i) {
        nebula::Realm* r = nebula::realm_at(i);
        char text[nebula::path::REALM_MAX + 3];
        u32 n = nebula::path::text_length(r->name());
        if (n > nebula::path::REALM_MAX) return Done::fail(Status::InvalidName);
        nebula::path::copy_text(text, r->name(), n);
        text[n] = ':';
        text[n + 1] = '/';
        text[n + 2] = 0;
        Result<Lens> root = mint(g_boot, text, Rights::all());
        if (!root.ok()) return Done::fail(root.status());
        g_boot_roots[g_boot_count] = root.value();
        g_boot_realms[g_boot_count] = r;
        ++g_boot_count;
    }
    return Done::success();
}

Lens boot_console() { return g_boot_console; }

Done mint_boot_console(Stream* console) {
    Result<Lens> c = mint_stream(g_boot, console, Right::Read | Right::Write | Right::Delegate);
    if (!c.ok()) return Done::fail(c.status());
    g_boot_console = c.value();
    return Done::success();
}

Lens boot_root(const char* realm) {
    for (u32 i = 0; i < g_boot_count; ++i)
        if (nebula::path::equal(g_boot_realms[i]->name(), realm)) return g_boot_roots[i];
    return Lens();
}

namespace text {

namespace {

using nebula::path::Path;

constexpr u32 REL_MAX = nebula::path::TEXT_MAX + 4;

nebula::Status to_nebula(Status s) {
    switch (s) {
        case Status::Ok: return nebula::Status::Ok;
        case Status::NotFound: return nebula::Status::NotFound;
        case Status::Exists: return nebula::Status::Exists;
        case Status::NotADirectory: return nebula::Status::NotADirectory;
        case Status::IsADirectory: return nebula::Status::IsADirectory;
        case Status::NotEmpty: return nebula::Status::NotEmpty;
        case Status::NoSpace: return nebula::Status::NoSpace;
        case Status::NoMemory: return nebula::Status::NoMemory;
        case Status::Io: return nebula::Status::Io;
        case Status::InvalidPath: return nebula::Status::InvalidPath;
        case Status::InvalidName: return nebula::Status::InvalidName;
        case Status::ReadOnly: return nebula::Status::ReadOnly;
        case Status::Busy: return nebula::Status::Busy;
        case Status::TooLarge: return nebula::Status::TooLarge;
        case Status::Unavailable: return nebula::Status::Unavailable;
        case Status::Unsupported: return nebula::Status::Unsupported;
        case Status::Corrupt: return nebula::Status::Corrupt;
        case Status::CrossRealm: return nebula::Status::CrossRealm;
        case Status::Denied:
        case Status::Escape: return nebula::Status::Denied;
        case Status::StaleLens:
        case Status::BadLens: return nebula::Status::BadHandle;
        case Status::TableFull: return nebula::Status::NoMemory;
        case Status::NotAFile: return nebula::Status::Unsupported;
        case Status::NoSuchEpoch: return nebula::Status::NotFound;
        case Status::TooManyEpochs: return nebula::Status::NoSpace;
        case Status::BatchFailed:
        case Status::Cancelled:
        case Status::Closed: return nebula::Status::Io;
        case Status::TimedOut:
        case Status::NotReady: return nebula::Status::Busy;
    }
    return nebula::Status::Io;
}

nebula::Status from_path_error(nebula::path::Error e) {
    switch (e) {
        case nebula::path::Error::Ok: return nebula::Status::Ok;
        case nebula::path::Error::BadName:
        case nebula::path::Error::NameTooLong: return nebula::Status::InvalidName;
        default: return nebula::Status::InvalidPath;
    }
}

class Held {
public:
    explicit Held(Cursor& c) : c_(c) {}
    ~Held() { release(); }
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;
    void release() {
        if (!l.is_null()) (void)lens::close(c_.domain, l);
        l = Lens();
    }
    Lens l;

private:
    Cursor& c_;
};

int root_index(const Cursor& c, const char* realm) {
    for (u32 i = 0; i < c.root_count; ++i)
        if (nebula::path::equal(c.realms[i]->name(), realm)) return static_cast<int>(i);
    return -1;
}

Rights rights_at(Cursor& c, Lens l) { return rights_of(c.domain, l).value_or(Rights()); }

void drop_stack(Cursor& c, u32 keep) {
    for (u32 i = keep + 1; i <= c.depth && i <= MAX_DEPTH; ++i) {
        (void)lens::close(c.domain, c.stack[i]);
        c.stack[i] = Lens();
    }
    if (c.depth > keep) c.depth = keep;
}

void rebuild(Cursor& c) {
    drop_stack(c, 0);
    int ri = root_index(c, c.here.realm);
    if (ri < 0) { c.here.count = 0; return; }
    c.stack[0] = c.roots[ri];
    for (u32 i = 0; i < c.here.count; ++i) {
        char rel[nebula::path::NAME_MAX + 3];
        u32 n = nebula::path::text_length(c.here.comps[i]);
        rel[0] = '.';
        rel[1] = '/';
        nebula::path::copy_text(rel + 2, c.here.comps[i], n);
        Result<Lens> next = open_at(c.domain, c.stack[i], rel, rights_at(c, c.stack[i]));
        if (!next.ok()) break;
        Result<NodeKind> k = kind_of(c.domain, next.value());
        if (!k.ok() || k.value() != NodeKind::Folder) { (void)lens::close(c.domain, next.value()); break; }
        c.stack[i + 1] = next.value();
        c.depth = i + 1;
    }
    c.here.count = c.depth;
}

void refresh(Cursor& c) {
    u32 gen = nebula::disk_generation();
    if (gen == c.volume_generation) return;
    int ri = root_index(c, c.here.realm);
    if (ri >= 0 && !c.realms[ri]->available()) return;
    c.volume_generation = gen;
    rebuild(c);
}

struct Target {
    Path path;
    nebula::Realm* realm;
    Lens base;
    u32 covered;
    char rel[REL_MAX];
};

bool has_colon(const char* s) {
    for (; *s; ++s)
        if (*s == ':') return true;
    return false;
}

nebula::Status aim(Cursor& c, const Path& p, bool relative, Target* t, bool entry) {
    int ri = root_index(c, p.realm);
    if (ri < 0) return nebula::Status::NoRealm;
    t->realm = c.realms[ri];
    if (!t->realm->available()) return nebula::Status::Unavailable;
    t->path = p;
    u32 k = 0;
    if (relative && nebula::path::equal(p.realm, c.here.realm)) {
        while (k < p.count && k < c.depth && nebula::path::equal(p.comps[k], c.here.comps[k])) ++k;
        if (entry && k == p.count && k > 0) --k;
        t->base = c.stack[k];
    } else {
        t->base = c.roots[ri];
    }
    t->covered = k;
    u32 need = 2;
    for (u32 i = k; i < p.count; ++i) need += nebula::path::text_length(p.comps[i]) + 1;
    if (need >= nebula::path::TEXT_MAX) return nebula::Status::InvalidPath;
    u32 n = 0;
    if (k == p.count) {
        t->rel[n++] = '.';
    } else {

        if (has_colon(p.comps[k])) { t->rel[n++] = '.'; t->rel[n++] = '/'; }
        for (u32 i = k; i < p.count; ++i) {
            if (i > k) t->rel[n++] = '/';
            u32 len = nebula::path::text_length(p.comps[i]);
            for (u32 j = 0; j < len; ++j) t->rel[n++] = p.comps[i][j];
        }
    }
    t->rel[n] = 0;
    return nebula::Status::Ok;
}

bool is_relative_text(const char* text) {
    if (text[0] == '/') return false;
    for (u32 i = 0; text[i] && text[i] != '/'; ++i)
        if (text[i] == ':') return false;
    return true;
}

nebula::Status locate(Cursor& c, const char* text, Target* t, bool entry = false) {
    refresh(c);
    Path p;
    nebula::path::Error e = nebula::path::resolve(c.here, text, p);
    if (e != nebula::path::Error::Ok) return from_path_error(e);
    return aim(c, p, is_relative_text(text), t, entry);
}

nebula::Status open_target(Cursor& c, const Target& t, Rights want, Held& h) {
    Result<Lens> l = open_at(c.domain, t.base, t.rel, want);
    if (!l.ok()) return to_nebula(l.status());
    h.l = l.value();
    return nebula::Status::Ok;
}

Rights look_rights(Cursor& c, const Target& t) { return rights_at(c, t.base) & RIGHTS_READ_ONLY; }

bool is_prefix(const Path& a, const Path& b) {
    if (!nebula::path::equal(a.realm, b.realm) || a.count > b.count) return false;
    for (u32 i = 0; i < a.count; ++i)
        if (!nebula::path::equal(a.comps[i], b.comps[i])) return false;
    return true;
}

void follow(Cursor& c, const Path& from, const Path* to) {
    if (!is_prefix(from, c.here)) return;
    if (to) {
        Path moved = *to;
        bool fits = true;
        for (u32 i = from.count; i < c.here.count && fits; ++i) fits = nebula::path::join(moved, c.here.comps[i]);
        if (fits) c.here = moved;
    }
    rebuild(c);
}

nebula::Status destination(Cursor& c, const char* to, const Path& src, Target* out) {
    Target t;
    nebula::Status st = locate(c, to, &t, true);
    if (st != nebula::Status::Ok) return st;
    Held h(c);
    nebula::Info inf;
    st = open_target(c, t, look_rights(c, t), h);
    if (st == nebula::Status::Ok) {
        Result<nebula::Info> ri = lens::stat(c.domain, h.l);
        if (!ri.ok()) return to_nebula(ri.status());
        inf = ri.value();
        h.release();
        if (inf.kind == nebula::Kind::Dir) {
            if (src.count == 0) return nebula::Status::InvalidPath;
            if (!nebula::path::join(t.path, src.comps[src.count - 1])) return nebula::Status::InvalidPath;
            st = aim(c, t.path, is_relative_text(to), &t, true);
            if (st != nebula::Status::Ok) return st;
        }
    } else if (st != nebula::Status::NotFound) {
        return st;
    }
    *out = t;
    return nebula::Status::Ok;
}

}

Done init(Cursor& c, const char* start_realm) {
    mem::zero(static_cast<void*>(&c), sizeof(c));
    for (u32 i = 0; i < nebula::realm_count() && i < nebula::MAX_REALMS; ++i) {
        nebula::Realm* r = nebula::realm_at(i);
        Lens boot = boot_root(r->name());
        if (boot.is_null()) continue;
        Result<Lens> mine = delegate(boot_domain(), boot, c.domain, Rights::all());
        if (!mine.ok()) { close(c); return Done::fail(mine.status()); }
        c.roots[c.root_count] = mine.value();
        c.realms[c.root_count] = r;
        ++c.root_count;
    }
    if (!boot_console().is_null()) {
        Result<Lens> con = delegate(boot_domain(), boot_console(), c.domain, Right::Read | Right::Write);
        if (!con.ok()) { close(c); return Done::fail(con.status()); }
        c.console = con.value();
    }
    u32 n = nebula::path::text_length(start_realm);
    if (n > nebula::path::REALM_MAX) n = nebula::path::REALM_MAX;
    nebula::path::copy_text(c.here.realm, start_realm, n);
    c.here.count = 0;
    int ri = root_index(c, c.here.realm);
    if (ri >= 0) c.stack[0] = c.roots[ri];
    c.volume_generation = nebula::disk_generation();
    c.ready = true;
    return Done::success();
}

Lens console(const Cursor& c) { return c.console; }

void close(Cursor& c) {
    close_all(c.domain);
    mem::zero(static_cast<void*>(&c), sizeof(c));
}

u32 location_text(const Cursor& c, char* out, u32 cap) { return nebula::path::to_text(c.here, out, cap); }

nebula::Status canonical(Cursor& c, const char* text, Path* out) {
    Path p;
    nebula::path::Error e = nebula::path::resolve(c.here, text, p);
    if (e != nebula::path::Error::Ok) return from_path_error(e);
    if (root_index(c, p.realm) < 0) return nebula::Status::NoRealm;
    *out = p;
    return nebula::Status::Ok;
}

nebula::Status change_location(Cursor& c, const char* text) {
    Target t;
    nebula::Status st = locate(c, text, &t);
    if (st != nebula::Status::Ok) return st;

    Lens fresh[MAX_DEPTH + 1];
    u32 opened = 0;
    Lens at = t.base;
    Rights rights = rights_at(c, t.base);
    st = nebula::Status::Ok;
    for (u32 i = t.covered; i < t.path.count && st == nebula::Status::Ok; ++i) {
        char rel[nebula::path::NAME_MAX + 3];
        u32 n = nebula::path::text_length(t.path.comps[i]);
        rel[0] = '.';
        rel[1] = '/';
        nebula::path::copy_text(rel + 2, t.path.comps[i], n);
        Result<Lens> next = open_at(c.domain, at, rel, rights);
        if (!next.ok()) { st = to_nebula(next.status()); break; }
        fresh[opened++] = next.value();
        at = next.value();
    }
    if (st == nebula::Status::Ok) {
        Result<NodeKind> k = kind_of(c.domain, at);
        if (!k.ok()) st = to_nebula(k.status());
        else if (k.value() != NodeKind::Folder) st = nebula::Status::NotADirectory;
    }
    if (st != nebula::Status::Ok) {
        for (u32 i = 0; i < opened; ++i) (void)lens::close(c.domain, fresh[i]);
        return st;
    }

    bool same_realm = nebula::path::equal(t.path.realm, c.here.realm);
    drop_stack(c, same_realm ? t.covered : 0);
    c.stack[0] = same_realm ? c.stack[0] : t.base;
    for (u32 i = 0; i < opened; ++i) c.stack[t.covered + 1 + i] = fresh[i];
    c.depth = t.path.count;
    c.here = t.path;
    return nebula::Status::Ok;
}

nebula::Status stat(Cursor& c, const char* text, nebula::Info* out) {
    Target t;
    nebula::Status st = locate(c, text, &t);
    if (st != nebula::Status::Ok) return st;
    Held h(c);
    st = open_target(c, t, look_rights(c, t), h);
    if (st != nebula::Status::Ok) return st;
    Result<nebula::Info> inf = lens::stat(c.domain, h.l);
    if (!inf.ok()) return to_nebula(inf.status());
    *out = inf.value();
    return nebula::Status::Ok;
}

nebula::Status stat_row(Cursor& c, const char* text, StatRow* out) {
    Target t;
    nebula::Status st = locate(c, text, &t);
    if (st != nebula::Status::Ok) return st;
    Held h(c);
    st = open_target(c, t, look_rights(c, t), h);
    if (st != nebula::Status::Ok) return st;
    Result<nebula::Info> inf = lens::stat(c.domain, h.l);
    if (!inf.ok()) return to_nebula(inf.status());
    out->info = inf.value();
    out->path = t.path;
    out->realm = t.realm->name();
    out->writable = t.realm->writable();
    return nebula::Status::Ok;
}

nebula::Status list(Cursor& c, const char* text, nebula::Listing* out, bool with_info) {
    *out = nebula::Listing{};
    Target t;
    nebula::Status st = locate(c, text, &t);
    if (st != nebula::Status::Ok) return st;
    Held h(c);
    st = open_target(c, t, Right::Enumerate, h);
    if (st != nebula::Status::Ok) return st;
    Done d = collect(c.domain, h.l, out, with_info, true);
    return d.ok() ? nebula::Status::Ok : to_nebula(d.status());
}

nebula::Status read_file(Cursor& c, const char* text, nebula::Blob* out) {
    *out = nebula::Blob{};
    Target t;
    nebula::Status st = locate(c, text, &t);
    if (st != nebula::Status::Ok) return st;
    Held h(c);
    st = open_target(c, t, Right::Read, h);
    if (st != nebula::Status::Ok) return st;
    Result<nebula::Blob> b = slurp(c.domain, h.l);
    if (!b.ok()) return to_nebula(b.status());
    *out = b.value();
    return nebula::Status::Ok;
}

nebula::Status write_file(Cursor& c, const char* text, const void* data, u64 size) {
    Target t;
    nebula::Status st = locate(c, text, &t, true);
    if (st != nebula::Status::Ok) return st;
    if (t.path.count == 0) return nebula::Status::IsADirectory;
    Done d = put(c.domain, t.base, t.rel, ConstBytes(static_cast<const u8*>(data), static_cast<usize>(size)));
    return d.ok() ? nebula::Status::Ok : to_nebula(d.status());
}

nebula::Status append_file(Cursor& c, const char* text, const void* data, u64 size) {
    Target t;
    nebula::Status st = locate(c, text, &t, true);
    if (st != nebula::Status::Ok) return st;
    if (t.path.count == 0) return nebula::Status::IsADirectory;
    Held h(c);
    st = open_target(c, t, Right::Append, h);
    if (st == nebula::Status::NotFound) {
        Result<Lens> made = create(c.domain, t.base, t.rel, Right::Append);
        if (!made.ok()) return to_nebula(made.status());
        h.l = made.value();
        st = nebula::Status::Ok;
    }
    if (st != nebula::Status::Ok) return st;
    Done d = append(c.domain, h.l, ConstBytes(static_cast<const u8*>(data), static_cast<usize>(size)));
    if (!d.ok()) return to_nebula(d.status());
    Done closed = lens::close(c.domain, h.l);
    h.l = Lens();
    return closed.ok() ? nebula::Status::Ok : to_nebula(closed.status());
}

nebula::Status make_dir(Cursor& c, const char* text) {
    Target t;
    nebula::Status st = locate(c, text, &t, true);
    if (st != nebula::Status::Ok) return st;
    if (t.path.count == 0) return nebula::Status::Exists;
    Done d = mkdir(c.domain, t.base, t.rel, false);
    return d.ok() ? nebula::Status::Ok : to_nebula(d.status());
}

nebula::Status make_path(Cursor& c, const char* text) {
    Target t;
    nebula::Status st = locate(c, text, &t, true);
    if (st != nebula::Status::Ok) return st;
    if (t.path.count == 0) return nebula::Status::Ok;
    Done d = mkdir(c.domain, t.base, t.rel, true);
    return d.ok() ? nebula::Status::Ok : to_nebula(d.status());
}

nebula::Status remove(Cursor& c, const char* text) {
    Target t;
    nebula::Status st = locate(c, text, &t, true);
    if (st != nebula::Status::Ok) return st;
    if (t.path.count == 0) return nebula::Status::Denied;
    Done d = unlink(c.domain, t.base, t.rel);
    if (d.ok()) follow(c, t.path, nullptr);
    return d.ok() ? nebula::Status::Ok : to_nebula(d.status());
}

nebula::Status remove_tree(Cursor& c, const char* text, u32* removed) {
    if (removed) *removed = 0;
    Target t;
    nebula::Status st = locate(c, text, &t, true);
    if (st != nebula::Status::Ok) return st;
    if (t.path.count == 0) return nebula::Status::Denied;
    Done d = lens::remove_tree(c.domain, t.base, t.rel, removed);
    if (d.ok()) follow(c, t.path, nullptr);
    return d.ok() ? nebula::Status::Ok : to_nebula(d.status());
}

nebula::Status rename(Cursor& c, const char* from, const char* to, u32 flags) {
    Target a;
    nebula::Status st = locate(c, from, &a, true);
    if (st != nebula::Status::Ok) return st;
    Held src(c);
    st = open_target(c, a, look_rights(c, a), src);
    if (st != nebula::Status::Ok) return st;
    if (a.path.count == 0) return nebula::Status::Denied;
    Result<nebula::Info> inf = lens::stat(c.domain, src.l);
    if (!inf.ok()) return to_nebula(inf.status());
    src.release();

    Target b;
    st = locate(c, to, &b, true);
    if (st != nebula::Status::Ok) return st;
    if (a.realm != b.realm) return nebula::Status::CrossRealm;
    if (b.path.count == 0) return nebula::Status::Denied;

    if (inf.value().kind == nebula::Kind::Dir && a.path.count != b.path.count && is_prefix(a.path, b.path))
        return nebula::Status::Unsupported;
    Done d = lens::rename(c.domain, a.base, a.rel, b.base, b.rel, (flags & nebula::RENAME_REPLACE) != 0);
    if (d.ok()) follow(c, a.path, &b.path);
    return d.ok() ? nebula::Status::Ok : to_nebula(d.status());
}

nebula::Status copy_file(Cursor& c, const char* from, const char* to) {
    Target a;
    nebula::Status st = locate(c, from, &a);
    if (st != nebula::Status::Ok) return st;
    Held src(c);
    st = open_target(c, a, Right::Read, src);
    if (st != nebula::Status::Ok) return st;
    Result<NodeKind> k = kind_of(c.domain, src.l);
    if (!k.ok()) return to_nebula(k.status());
    if (k.value() == NodeKind::Folder) return nebula::Status::IsADirectory;
    Target b;
    st = destination(c, to, a.path, &b);
    if (st != nebula::Status::Ok) return st;
    if (b.path.count == 0) return nebula::Status::IsADirectory;
    Done d = copy(c.domain, src.l, b.base, b.rel);
    return d.ok() ? nebula::Status::Ok : to_nebula(d.status());
}

nebula::Status copy_tree(Cursor& c, const char* from, const char* to, u32* copied) {
    if (copied) *copied = 0;
    Target a;
    nebula::Status st = locate(c, from, &a);
    if (st != nebula::Status::Ok) return st;
    Held src(c);
    st = open_target(c, a, Right::Read | Right::Enumerate, src);
    if (st != nebula::Status::Ok) return st;
    Result<NodeKind> k = kind_of(c.domain, src.l);
    if (!k.ok()) return to_nebula(k.status());
    if (k.value() != NodeKind::Folder) {
        src.release();
        st = copy_file(c, from, to);
        if (st == nebula::Status::Ok && copied) *copied = 1;
        return st;
    }
    Target b;
    st = destination(c, to, a.path, &b);
    if (st != nebula::Status::Ok) return st;
    if (is_prefix(a.path, b.path)) return nebula::Status::Unsupported;
    if (b.path.count == 0) return nebula::Status::InvalidPath;
    Done d = lens::copy_tree(c.domain, src.l, b.base, b.rel, copied);
    return d.ok() ? nebula::Status::Ok : to_nebula(d.status());
}

Result<Lens> grant(Cursor& c, const char* text, Rights rights, bool read_only) {
    Target t;
    nebula::Status st = locate(c, text, &t);
    if (st != nebula::Status::Ok) return Result<Lens>::fail(from_nebula(st));
    Rights held = rights_at(c, t.base);
    Rights want = (rights | Right::Delegate) & held;
    Result<Lens> l = open_at(c.domain, t.base, t.rel, want);
    if (!l.ok() || !read_only) return l;
    Result<Lens> ro = narrow(c.domain, l.value(), Rights::all(), true);
    (void)lens::close(c.domain, l.value());
    return ro;
}

}
}
