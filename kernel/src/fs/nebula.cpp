#include <cosmos/nebula.hpp>
#include <cosmos/kheap.hpp>
#include <cosmos/orbital.hpp>

namespace nebula {

namespace {

constexpr u32 MAX_WRITERS = 64;
constexpr u64 MIN_BUFFER = 4096;

struct Writer {
    Realm* realm;
    Node node;
};

Realm* g_realms[MAX_REALMS];
u32 g_realm_count;
Writer g_writers[MAX_WRITERS];
orbital::Mutex g_lock;

bool same_node(Node a, Node b) { return a.id == b.id && a.scope == b.scope; }

bool writer_busy_locked(Realm* realm, Node node) {
    for (u32 i = 0; i < MAX_WRITERS; ++i)
        if (g_writers[i].realm == realm && same_node(g_writers[i].node, node)) return true;
    return false;
}

bool writer_busy(Realm* realm, Node node) {
    g_lock.lock();
    bool busy = writer_busy_locked(realm, node);
    g_lock.unlock();
    return busy;
}

bool writer_claim(Realm* realm, Node node) {
    g_lock.lock();
    bool ok = false;
    if (!writer_busy_locked(realm, node)) {
        for (u32 i = 0; i < MAX_WRITERS; ++i) {
            if (!g_writers[i].realm) {
                g_writers[i].realm = realm;
                g_writers[i].node = node;
                ok = true;
                break;
            }
        }
    }
    g_lock.unlock();
    return ok;
}

void writer_release(Realm* realm, Node node) {
    g_lock.lock();
    for (u32 i = 0; i < MAX_WRITERS; ++i) {
        if (g_writers[i].realm == realm && same_node(g_writers[i].node, node)) {
            g_writers[i].realm = nullptr;
            g_writers[i].node = Node{0, 0};
            break;
        }
    }
    g_lock.unlock();
}

Status from_path_error(path::Error e) {
    switch (e) {
        case path::Error::BadName:
        case path::Error::NameTooLong: return Status::InvalidName;
        case path::Error::Ok: return Status::Ok;
        default: return Status::InvalidPath;
    }
}

void copy_name(char* dst, const char* src) {
    u32 i = 0;
    for (; src[i] && i < NAME_BYTES; ++i) dst[i] = src[i];
    dst[i] = 0;
}

int compare_names(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    u8 x = static_cast<u8>(*a), y = static_cast<u8>(*b);
    return x < y ? -1 : (x > y ? 1 : 0);
}

bool entry_before(const Entry& a, const Entry& b) {
    if (a.kind != b.kind) return a.kind == Kind::Dir;
    return compare_names(a.name, b.name) < 0;
}

void sort_entries(Entry* v, u32 n) {
    u32 gap = 1;
    while (gap < n / 3) gap = gap * 3 + 1;
    for (; gap > 0; gap = (gap - 1) / 3) {
        for (u32 i = gap; i < n; ++i) {
            Entry tmp = v[i];
            u32 j = i;
            while (j >= gap && entry_before(tmp, v[j - gap])) {
                v[j] = v[j - gap];
                j -= gap;
            }
            v[j] = tmp;
        }
    }
}

struct NameItem {
    char name[NAME_BYTES + 1];
    Kind kind;
    Node node;
};

struct NameList {
    NameItem* items;
    u32 count;
    u32 capacity;
    bool failed;
    bool truncated;
};

void name_list_free(NameList& l) {
    if (l.items) kheap::free(l.items);
    l = NameList{};
}

void name_list_add(const char* name, Kind kind, Node node, void* ctx) {
    NameList& l = *static_cast<NameList*>(ctx);
    if (l.failed) return;
    if (l.count >= MAX_LIST_ENTRIES) { l.truncated = true; return; }
    if (l.count == l.capacity) {
        u32 cap = l.capacity ? l.capacity * 2 : 64;
        NameItem* bigger = static_cast<NameItem*>(kheap::alloc(static_cast<usize>(cap) * sizeof(NameItem)));
        if (!bigger) { l.failed = true; return; }
        for (u32 i = 0; i < l.count; ++i) bigger[i] = l.items[i];
        if (l.items) kheap::free(l.items);
        l.items = bigger;
        l.capacity = cap;
    }
    NameItem& it = l.items[l.count++];
    copy_name(it.name, name);
    it.kind = kind;
    it.node = node;
}

Status collect(Realm* realm, Node dir, NameList& out) {
    out = NameList{};
    Status st = realm->list(dir, &name_list_add, &out);
    if (st != Status::Ok) { name_list_free(out); return st; }
    if (out.failed) { name_list_free(out); return Status::NoMemory; }
    return Status::Ok;
}

Status resolve_canonical(const path::Path& p, Resolved* out) {
    out->path = p;
    Realm* r = find_realm(p.realm);
    if (!r) return Status::NoRealm;
    if (!r->available()) return Status::Unavailable;
    out->realm = r;
    out->node = r->root();
    out->parent = out->node;
    out->exists = true;
    out->is_root = p.count == 0;
    out->leaf[0] = 0;
    for (u32 i = 0; i < p.count; ++i) {
        Info inf;
        Status st = r->info(out->node, &inf);
        if (st != Status::Ok) return st;
        if (inf.kind != Kind::Dir) return Status::NotADirectory;
        out->parent = out->node;
        copy_name(out->leaf, p.comps[i]);
        Node next;
        st = r->find(out->node, p.comps[i], &next);
        if (st == Status::NotFound) {
            if (i + 1 == p.count) { out->exists = false; return Status::Ok; }
            return Status::NotFound;
        }
        if (st != Status::Ok) return st;
        out->node = next;
    }
    return Status::Ok;
}

bool is_prefix(const path::Path& a, const path::Path& b) {
    if (!path::equal(a.realm, b.realm) || a.count > b.count) return false;
    for (u32 i = 0; i < a.count; ++i)
        if (!path::equal(a.comps[i], b.comps[i])) return false;
    return true;
}

OpenFile* lookup(Session& s, Handle h) {
    u32 index = (h & 0xFFFF);
    u16 gen = static_cast<u16>(h >> 16);
    if (index == 0 || index > MAX_HANDLES) return nullptr;
    OpenFile& f = s.files[index - 1];
    if (!f.used || f.generation != gen) return nullptr;
    return &f;
}

Status ensure_capacity(OpenFile& f, u64 need) {
    if (need > MAX_FILE_BYTES) return Status::TooLarge;
    if (need <= f.capacity) return Status::Ok;
    u64 cap = f.capacity * 2;
    if (cap < need) cap = need;
    if (cap < MIN_BUFFER) cap = MIN_BUFFER;
    if (cap > MAX_FILE_BYTES) cap = MAX_FILE_BYTES;
    u8* bigger = static_cast<u8*>(kheap::alloc(static_cast<usize>(cap)));
    if (!bigger) return Status::NoMemory;
    if (f.size) mem::copy(bigger, f.data, static_cast<usize>(f.size));
    if (f.data) kheap::free(f.data);
    f.data = bigger;
    f.capacity = cap;
    return Status::Ok;
}

Status slurp(Realm* realm, Node node, const Info& inf, u8** out, u64* got) {
    *out = nullptr;
    *got = 0;
    if (inf.size > MAX_FILE_BYTES) return Status::TooLarge;
    u64 cap = inf.size + (inf.dynamic ? DYNAMIC_SLACK : 0);
    if (cap == 0) return Status::Ok;
    u8* buf = static_cast<u8*>(kheap::alloc(static_cast<usize>(cap)));
    if (!buf) return Status::NoMemory;
    u64 n = 0;
    Status st = realm->read_all(node, buf, cap, &n);
    if (st != Status::Ok || n > cap || (!inf.dynamic && n != inf.size)) {
        kheap::free(buf);
        return st != Status::Ok ? st : Status::Io;
    }
    *out = buf;
    *got = n;
    return Status::Ok;
}

Status store_node_impl(Realm* realm, Node parent, const char* leaf, bool exists, Node node, const void* data, u64 size) {
    if (size > MAX_FILE_BYTES) return Status::TooLarge;
    if (!realm->writable()) return Status::ReadOnly;
    bool created = false;
    Status st;
    if (!exists) {
        st = realm->create_file(parent, leaf, &node);
        if (st != Status::Ok) return st;
        created = true;
    } else {
        Info inf;
        st = realm->info(node, &inf);
        if (st != Status::Ok) return st;
        if (inf.kind == Kind::Dir) return Status::IsADirectory;
    }
    if (!writer_claim(realm, node)) return Status::Busy;
    st = realm->write_all(node, data, size);
    writer_release(realm, node);
    if (st != Status::Ok && created) realm->remove(parent, leaf);
    return st;
}

Status store(const path::Path& p, const void* data, u64 size) {
    if (size > MAX_FILE_BYTES) return Status::TooLarge;
    Resolved r;
    Status st = resolve_canonical(p, &r);
    if (st != Status::Ok) return st;
    if (!r.realm->writable()) return Status::ReadOnly;
    if (r.is_root) return Status::IsADirectory;
    return store_node_impl(r.realm, r.parent, r.leaf, r.exists, r.node, data, size);
}

}

const char* status_text(Status s) {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::NotFound: return "not found";
        case Status::Exists: return "already exists";
        case Status::NotADirectory: return "not a directory";
        case Status::IsADirectory: return "is a directory";
        case Status::NotEmpty: return "directory is not empty";
        case Status::NoSpace: return "no space left";
        case Status::NoMemory: return "out of memory";
        case Status::Io: return "input/output error";
        case Status::InvalidPath: return "invalid path";
        case Status::InvalidName: return "invalid name (UTF-8 only, no control characters, at most 51 bytes)";
        case Status::ReadOnly: return "read-only realm";
        case Status::Busy: return "busy (open for writing elsewhere)";
        case Status::TooLarge: return "file too large (16 MiB limit)";
        case Status::NoRealm: return "no such realm";
        case Status::Unavailable: return "realm is not available";
        case Status::BadHandle: return "bad handle";
        case Status::CrossRealm: return "cannot move between realms";
        case Status::Unsupported: return "not supported";
        case Status::Corrupt: return "data failed its checksum";
        case Status::Denied: return "not allowed";
    }
    return "unknown";
}

bool register_realm(Realm* realm) {
    if (!realm || g_realm_count >= MAX_REALMS) return false;
    for (u32 i = 0; i < g_realm_count; ++i)
        if (path::equal(g_realms[i]->name(), realm->name())) return false;
    g_realms[g_realm_count++] = realm;
    return true;
}

Realm* find_realm(const char* name) {
    for (u32 i = 0; i < g_realm_count; ++i)
        if (path::equal(g_realms[i]->name(), name)) return g_realms[i];
    return nullptr;
}

u32 realm_count() { return g_realm_count; }
Realm* realm_at(u32 index) { return index < g_realm_count ? g_realms[index] : nullptr; }

void init_session(Session& s, const char* realm) {
    mem::zero(&s, sizeof(s));
    path::copy_text(s.here.realm, realm, path::text_length(realm) > path::REALM_MAX ? path::REALM_MAX : path::text_length(realm));
    s.here.count = 0;
    s.ready = true;
}

void close_session(Session& s) {
    for (u32 i = 0; i < MAX_HANDLES; ++i) {
        if (!s.files[i].used) continue;
        Handle h = (static_cast<u32>(s.files[i].generation) << 16) | (i + 1);
        close(s, h);
    }
}

Status resolve(Session& s, const char* text, Resolved* out) {
    path::Path p;
    path::Error e = path::resolve(s.here, text, p);
    if (e != path::Error::Ok) return from_path_error(e);
    return resolve_canonical(p, out);
}

Status change_location(Session& s, const char* text) {
    Resolved r;
    Status st = resolve(s, text, &r);
    if (st != Status::Ok) return st;
    if (!r.exists) return Status::NotFound;
    Info inf;
    st = r.realm->info(r.node, &inf);
    if (st != Status::Ok) return st;
    if (inf.kind != Kind::Dir) return Status::NotADirectory;
    s.here = r.path;
    return Status::Ok;
}

u32 location_text(const Session& s, char* out, u32 cap) { return path::to_text(s.here, out, cap); }

Status stat(Session& s, const char* text, Info* out) {
    Resolved r;
    Status st = resolve(s, text, &r);
    if (st != Status::Ok) return st;
    if (!r.exists) return Status::NotFound;
    return r.realm->info(r.node, out);
}

Status list_node(Realm* realm, Node dir, Listing* out, bool with_info, bool sorted) {
    *out = Listing{};
    NameList names;
    Status st = collect(realm, dir, names);
    if (st != Status::Ok) return st;
    if (names.count) {
        out->items = static_cast<Entry*>(kheap::alloc(static_cast<usize>(names.count) * sizeof(Entry)));
        if (!out->items) { name_list_free(names); return Status::NoMemory; }
        out->capacity = names.count;
    }
    for (u32 i = 0; i < names.count; ++i) {
        Entry& e = out->items[out->count++];
        copy_name(e.name, names.items[i].name);
        e.kind = names.items[i].kind;
        e.size = 0;
        e.mtime_ms = 0;
        if (with_info) {
            Info ci;
            if (realm->info(names.items[i].node, &ci) == Status::Ok) {
                e.size = ci.size;
                e.mtime_ms = ci.mtime_ms;
            }
        }
    }
    out->truncated = names.truncated;
    name_list_free(names);
    if (sorted) sort_entries(out->items, out->count);
    return Status::Ok;
}

Status list(Session& s, const char* text, Listing* out, bool with_info) {
    *out = Listing{};
    Resolved r;
    Status st = resolve(s, text, &r);
    if (st != Status::Ok) return st;
    if (!r.exists) return Status::NotFound;
    Info inf;
    st = r.realm->info(r.node, &inf);
    if (st != Status::Ok) return st;
    if (inf.kind != Kind::Dir) return Status::NotADirectory;
    return list_node(r.realm, r.node, out, with_info, true);
}

void release(Listing& l) {
    if (l.items) kheap::free(l.items);
    l = Listing{};
}

static Status make_open_file(OpenFile* out, Realm* realm, Node node, u32 flags, const Info& binfo) {
    bool write = (flags & OPEN_WRITE) != 0;
    bool truncate = write && (flags & OPEN_TRUNCATE);
    if (write && !writer_claim(realm, node)) return Status::Busy;

    OpenFile f{};
    f.realm = realm;
    f.node = node;
    f.flags = flags;
    if (truncate) {
        f.dirty = true;
    } else if (!write && !binfo.dynamic && realm->ranged()) {
        u64 version = 0;
        u64 unused = 0;
        Status st = realm->read_range(node, 0, nullptr, 0, &unused, &version);
        if (st != Status::Ok) return st;
        f.ranged = true;
        f.version = version;
        f.size = binfo.size;
    } else if (binfo.size > 0 || binfo.dynamic) {
        u8* buf = nullptr;
        u64 got = 0;
        Status st = slurp(realm, node, binfo, &buf, &got);
        if (st != Status::Ok) {
            if (write) writer_release(realm, node);
            return st;
        }
        f.data = buf;
        f.capacity = binfo.size + (binfo.dynamic ? DYNAMIC_SLACK : 0);
        f.size = got;
        if (!buf) f.capacity = 0;
    }
    *out = f;
    return Status::Ok;
}

Status open(Session& s, const char* text, u32 flags, Handle* out) {
    *out = NO_HANDLE;
    if (!(flags & (OPEN_READ | OPEN_WRITE))) return Status::Unsupported;
    bool write = (flags & OPEN_WRITE) != 0;
    Resolved r;
    Status st = resolve(s, text, &r);
    if (st != Status::Ok) return st;

    u32 slot = MAX_HANDLES;
    for (u32 i = 0; i < MAX_HANDLES; ++i)
        if (!s.files[i].used) { slot = i; break; }
    if (slot == MAX_HANDLES) return Status::Busy;

    Node node = r.node;
    Info binfo{};
    OpenFile f_{};
    bool created = false;
    if (!r.exists) {
        if (!(flags & OPEN_CREATE) || !write) return Status::NotFound;
        if (!r.realm->writable()) return Status::ReadOnly;
        st = r.realm->create_file(r.parent, r.leaf, &node);
        if (st != Status::Ok) return st;
        created = true;
    } else {
        if ((flags & OPEN_CREATE) && (flags & OPEN_EXCLUSIVE)) return Status::Exists;
        Info inf;
        st = r.realm->info(node, &inf);
        if (st != Status::Ok) return st;
        if (inf.kind == Kind::Dir) return Status::IsADirectory;
        if (write && !r.realm->writable()) return Status::ReadOnly;
        binfo = inf;
    }

    st = make_open_file(&f_, r.realm, node, flags, binfo);
    if (st != Status::Ok) {
        if (created) r.realm->remove(r.parent, r.leaf);
        return st;
    }

    OpenFile& dest = s.files[slot];
    u16 gen = static_cast<u16>(dest.generation + 1);
    if (gen == 0) gen = 1;
    f_.used = true;
    f_.generation = gen;
    dest = f_;
    *out = (static_cast<u32>(gen) << 16) | (slot + 1);
    return Status::Ok;
}

Status file_read(OpenFile& f, u64 offset, void* buf, u64 length, u64* got) {
    if (!(f.flags & OPEN_READ)) return Status::Denied;
    *got = 0;
    if (offset >= f.size || length == 0) return Status::Ok;
    u64 n = f.size - offset;
    if (n > length) n = length;
    if (f.ranged) return f.realm->read_range(f.node, offset, buf, n, got, &f.version);
    mem::copy(buf, f.data + offset, static_cast<usize>(n));
    *got = n;
    return Status::Ok;
}

static Status grow_zeroed(OpenFile& f, u64 new_size) {
    Status st = ensure_capacity(f, new_size);
    if (st != Status::Ok) return st;
    if (new_size > f.size) mem::zero(f.data + f.size, static_cast<usize>(new_size - f.size));
    f.size = new_size;
    f.dirty = true;
    return Status::Ok;
}

Status file_write(OpenFile& f, u64 offset, const void* data, u64 length) {
    if (!(f.flags & OPEN_WRITE)) return Status::Denied;
    if (length == 0) return Status::Ok;
    if (offset > MAX_FILE_BYTES || length > MAX_FILE_BYTES - offset) return Status::TooLarge;
    u64 end = offset + length;
    if (end > f.size) {
        Status st = grow_zeroed(f, end);
        if (st != Status::Ok) return st;
    }
    mem::copy(f.data + offset, data, static_cast<usize>(length));
    f.dirty = true;
    return Status::Ok;
}

Status file_append(OpenFile& f, const void* data, u64 length) { return file_write(f, f.size, data, length); }

Status file_resize(OpenFile& f, u64 new_size) {
    if (!(f.flags & OPEN_WRITE)) return Status::Denied;
    if (new_size > MAX_FILE_BYTES) return Status::TooLarge;
    if (new_size > f.size) return grow_zeroed(f, new_size);
    if (new_size < f.size) {
        f.size = new_size;
        f.dirty = true;
    }
    return Status::Ok;
}

Status file_sync(OpenFile& f) {
    if (!(f.flags & OPEN_WRITE) || !f.dirty) return Status::Ok;
    Status st = f.realm->write_all(f.node, f.data, f.size);
    if (st == Status::Ok) f.dirty = false;
    return st;
}

Status file_close(OpenFile& f) {
    Status st = Status::Ok;
    if ((f.flags & OPEN_WRITE)) {
        st = file_sync(f);
        writer_release(f.realm, f.node);
    }
    if (f.data) kheap::free(f.data);
    u16 gen = f.generation;
    f = OpenFile{};
    f.generation = gen;
    return st;
}

Status read_at(Session& s, Handle h, u64 offset, void* buf, u64 length, u64* got) {
    OpenFile* f = lookup(s, h);
    if (!f) return Status::BadHandle;
    return file_read(*f, offset, buf, length, got);
}

Status write_at(Session& s, Handle h, u64 offset, const void* data, u64 length) {
    OpenFile* f = lookup(s, h);
    if (!f) return Status::BadHandle;
    return file_write(*f, offset, data, length);
}

Status append(Session& s, Handle h, const void* data, u64 length) {
    OpenFile* f = lookup(s, h);
    if (!f) return Status::BadHandle;
    return file_append(*f, data, length);
}

Status size_of(Session& s, Handle h, u64* out) {
    OpenFile* f = lookup(s, h);
    if (!f) return Status::BadHandle;
    *out = f->size;
    return Status::Ok;
}

Status resize(Session& s, Handle h, u64 new_size) {
    OpenFile* f = lookup(s, h);
    if (!f) return Status::BadHandle;
    return file_resize(*f, new_size);
}

Status sync(Session& s, Handle h) {
    OpenFile* f = lookup(s, h);
    if (!f) return Status::BadHandle;
    return file_sync(*f);
}

Status close(Session& s, Handle h) {
    OpenFile* f = lookup(s, h);
    if (!f) return Status::BadHandle;
    return file_close(*f);
}

Status read_file(Session& s, const char* text, Blob* out) {
    *out = Blob{};
    Resolved r;
    Status st = resolve(s, text, &r);
    if (st != Status::Ok) return st;
    if (!r.exists) return Status::NotFound;
    Info inf;
    st = r.realm->info(r.node, &inf);
    if (st != Status::Ok) return st;
    if (inf.kind == Kind::Dir) return Status::IsADirectory;
    u8* buf = nullptr;
    u64 got = 0;
    st = slurp(r.realm, r.node, inf, &buf, &got);
    if (st != Status::Ok) return st;
    out->data = buf;
    out->size = got;
    return Status::Ok;
}

void release(Blob& b) {
    if (b.data) kheap::free(b.data);
    b = Blob{};
}

Status write_file(Session& s, const char* text, const void* data, u64 size) {
    path::Path p;
    path::Error e = path::resolve(s.here, text, p);
    if (e != path::Error::Ok) return from_path_error(e);
    return store(p, data, size);
}

Status append_file(Session& s, const char* text, const void* data, u64 size) {
    Handle h;
    Status st = open(s, text, OPEN_READ | OPEN_WRITE | OPEN_CREATE, &h);
    if (st != Status::Ok) return st;
    st = append(s, h, data, size);
    Status closed = close(s, h);
    return st != Status::Ok ? st : closed;
}

Status make_dir(Session& s, const char* text) {
    Resolved r;
    Status st = resolve(s, text, &r);
    if (st != Status::Ok) return st;
    if (r.exists) return Status::Exists;
    if (!r.realm->writable()) return Status::ReadOnly;
    Node created;
    return r.realm->make_dir(r.parent, r.leaf, &created);
}

static Status make_dirs(const path::Path& p) {
    Realm* realm = find_realm(p.realm);
    if (!realm) return Status::NoRealm;
    if (!realm->available()) return Status::Unavailable;
    Node node = realm->root();
    for (u32 i = 0; i < p.count; ++i) {
        Info inf;
        Status st = realm->info(node, &inf);
        if (st != Status::Ok) return st;
        if (inf.kind != Kind::Dir) return Status::NotADirectory;
        Node next;
        st = realm->find(node, p.comps[i], &next);
        if (st == Status::NotFound) {
            if (!realm->writable()) return Status::ReadOnly;
            st = realm->make_dir(node, p.comps[i], &next);
        }
        if (st != Status::Ok) return st;
        node = next;
    }
    Info last;
    Status st = realm->info(node, &last);
    if (st != Status::Ok) return st;
    return last.kind == Kind::Dir ? Status::Ok : Status::NotADirectory;
}

Status make_path(Session& s, const char* text) {
    path::Path p;
    path::Error e = path::resolve(s.here, text, p);
    if (e != path::Error::Ok) return from_path_error(e);
    return make_dirs(p);
}

Status remove(Session& s, const char* text) {
    Resolved r;
    Status st = resolve(s, text, &r);
    if (st != Status::Ok) return st;
    if (!r.exists) return Status::NotFound;
    if (r.is_root) return Status::Denied;
    if (!r.realm->writable()) return Status::ReadOnly;
    if (writer_busy(r.realm, r.node)) return Status::Busy;
    return r.realm->remove(r.parent, r.leaf);
}

static Status remove_below(Realm* realm, Node dir, u32 depth, u32* removed) {
    if (depth > MAX_TREE_DEPTH) return Status::Unsupported;
    NameList names;
    Status st = collect(realm, dir, names);
    if (st != Status::Ok) return st;
    for (u32 i = 0; i < names.count && st == Status::Ok; ++i) {
        NameItem& it = names.items[i];
        if (it.kind == Kind::Dir) {
            st = remove_below(realm, it.node, depth + 1, removed);
        } else if (writer_busy(realm, it.node)) {
            st = Status::Busy;
        }
        if (st != Status::Ok) break;
        st = realm->remove(dir, it.name);
        if (st == Status::Ok && removed) ++*removed;
    }
    name_list_free(names);
    return st;
}

Status remove_tree(Session& s, const char* text, u32* removed) {
    if (removed) *removed = 0;
    Resolved r;
    Status st = resolve(s, text, &r);
    if (st != Status::Ok) return st;
    if (!r.exists) return Status::NotFound;
    if (r.is_root) return Status::Denied;
    if (!r.realm->writable()) return Status::ReadOnly;
    Info inf;
    st = r.realm->info(r.node, &inf);
    if (st != Status::Ok) return st;
    if (inf.kind == Kind::Dir) {
        st = remove_below(r.realm, r.node, 0, removed);
        if (st != Status::Ok) return st;
    } else if (writer_busy(r.realm, r.node)) {
        return Status::Busy;
    }
    st = r.realm->remove(r.parent, r.leaf);
    if (st == Status::Ok && removed) ++*removed;
    return st;
}

Status rename(Session& s, const char* from, const char* to, u32 flags) {
    Resolved a, b;
    Status st = resolve(s, from, &a);
    if (st != Status::Ok) return st;
    if (!a.exists) return Status::NotFound;
    if (a.is_root) return Status::Denied;
    st = resolve(s, to, &b);
    if (st != Status::Ok) return st;
    if (a.realm != b.realm) return Status::CrossRealm;
    if (b.is_root) return Status::Denied;
    if (!a.realm->writable()) return Status::ReadOnly;
    bool replace = (flags & RENAME_REPLACE) != 0;
    if (b.exists && same_node(a.node, b.node)) return Status::Ok;
    if (b.exists && !replace) return Status::Exists;
    Info inf;
    st = a.realm->info(a.node, &inf);
    if (st != Status::Ok) return st;
    if (inf.kind == Kind::Dir && is_prefix(a.path, b.path)) return Status::Unsupported;
    if (writer_busy(a.realm, a.node)) return Status::Busy;
    if (b.exists && writer_busy(b.realm, b.node)) return Status::Busy;
    return a.realm->rename(a.parent, a.leaf, b.parent, b.leaf, replace);
}

static Status target_path(Session& s, const Resolved& from, const char* to, path::Path* out) {
    path::Path p;
    path::Error e = path::resolve(s.here, to, p);
    if (e != path::Error::Ok) return from_path_error(e);
    Resolved t;
    Status st = resolve_canonical(p, &t);
    if (st != Status::Ok) return st;
    if (t.exists) {
        Info inf;
        st = t.realm->info(t.node, &inf);
        if (st != Status::Ok) return st;
        if (inf.kind == Kind::Dir) {
            if (from.is_root || !path::join(p, from.leaf)) return Status::InvalidPath;
        }
    }
    *out = p;
    return Status::Ok;
}

Status copy_file(Session& s, const char* from, const char* to) {
    Resolved a;
    Status st = resolve(s, from, &a);
    if (st != Status::Ok) return st;
    if (!a.exists) return Status::NotFound;
    Info inf;
    st = a.realm->info(a.node, &inf);
    if (st != Status::Ok) return st;
    if (inf.kind == Kind::Dir) return Status::IsADirectory;
    path::Path dest;
    st = target_path(s, a, to, &dest);
    if (st != Status::Ok) return st;
    Resolved d;
    st = resolve_canonical(dest, &d);
    if (st != Status::Ok) return st;
    if (d.exists && d.realm == a.realm && same_node(d.node, a.node)) return Status::Exists;
    u8* buf = nullptr;
    u64 got = 0;
    st = slurp(a.realm, a.node, inf, &buf, &got);
    if (st != Status::Ok) return st;
    st = store(dest, buf, got);
    if (buf) kheap::free(buf);
    return st;
}

static Status copy_below(Realm* realm, Node dir, path::Path& src, path::Path& dst, u32 depth, u32* copied) {
    if (depth > MAX_TREE_DEPTH) return Status::Unsupported;
    NameList names;
    Status st = collect(realm, dir, names);
    if (st != Status::Ok) return st;
    for (u32 i = 0; i < names.count && st == Status::Ok; ++i) {
        NameItem& it = names.items[i];
        if (!path::join(src, it.name)) { st = Status::InvalidPath; break; }
        if (!path::join(dst, it.name)) { --src.count; st = Status::InvalidPath; break; }
        if (it.kind == Kind::Dir) {
            st = make_dirs(dst);
            if (st == Status::Ok) st = copy_below(realm, it.node, src, dst, depth + 1, copied);
        } else {
            Info inf;
            st = realm->info(it.node, &inf);
            u8* buf = nullptr;
            u64 got = 0;
            if (st == Status::Ok) st = slurp(realm, it.node, inf, &buf, &got);
            if (st == Status::Ok) {
                st = store(dst, buf, got);
                if (st == Status::Ok && copied) ++*copied;
            }
            if (buf) kheap::free(buf);
        }
        --src.count;
        --dst.count;
    }
    name_list_free(names);
    return st;
}

Status copy_tree(Session& s, const char* from, const char* to, u32* copied) {
    if (copied) *copied = 0;
    Resolved a;
    Status st = resolve(s, from, &a);
    if (st != Status::Ok) return st;
    if (!a.exists) return Status::NotFound;
    Info inf;
    st = a.realm->info(a.node, &inf);
    if (st != Status::Ok) return st;
    if (inf.kind == Kind::File) {
        st = copy_file(s, from, to);
        if (st == Status::Ok && copied) *copied = 1;
        return st;
    }
    path::Path dest;
    st = target_path(s, a, to, &dest);
    if (st != Status::Ok) return st;
    if (is_prefix(a.path, dest)) return Status::Unsupported;
    st = make_dirs(dest);
    if (st != Status::Ok) return st;
    path::Path src = a.path;
    return copy_below(a.realm, a.node, src, dest, 0, copied);
}

bool node_busy(Realm* realm, Node node) { return writer_busy(realm, node); }

Status open_node(OpenFile& out, Realm* realm, Node node, u32 flags) {
    out = OpenFile{};
    if (!(flags & (OPEN_READ | OPEN_WRITE))) return Status::Unsupported;
    if (!realm->available()) return Status::Unavailable;
    bool write = (flags & OPEN_WRITE) != 0;
    Info inf;
    Status st = realm->info(node, &inf);
    if (st != Status::Ok) return st;
    if (inf.kind == Kind::Dir) return Status::IsADirectory;
    if (write && (!realm->writable() || inf.read_only)) return Status::ReadOnly;
    st = make_open_file(&out, realm, node, flags, inf);
    if (st != Status::Ok) return st;
    out.used = true;
    return Status::Ok;
}

Status read_node(Realm* realm, Node file, Blob* out) {
    *out = Blob{};
    Info inf;
    Status st = realm->info(file, &inf);
    if (st != Status::Ok) return st;
    if (inf.kind == Kind::Dir) return Status::IsADirectory;
    u8* buf = nullptr;
    u64 got = 0;
    st = slurp(realm, file, inf, &buf, &got);
    if (st != Status::Ok) return st;
    out->data = buf;
    out->size = got;
    return Status::Ok;
}

Status store_node(Realm* realm, Node parent, const char* leaf, bool exists, Node node, const void* data, u64 size) {
    return store_node_impl(realm, parent, leaf, exists, node, data, size);
}

}
