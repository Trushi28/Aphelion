#include <cosmos/nebula_realms.hpp>
#include <cosmos/kheap.hpp>

namespace nebula {

namespace {

bool g_mounted;
bool g_confirmed_only;

struct ListCtx {
    ChildFn fn;
    void* ctx;
    u64 snap;
};

void list_bridge(const char* name, u64 star, u32 type, void* raw) {
    ListCtx& c = *static_cast<ListCtx*>(raw);
    if (type != stellar::TYPE_FILE && type != stellar::TYPE_CONSTELLATION) return;
    c.fn(name, type == stellar::TYPE_CONSTELLATION ? Kind::Dir : Kind::File, Node{star, c.snap}, c.ctx);
}

Status fill_info(u64 star, u64 snap, Info* out, bool read_only) {
    stellar::StatInfo si;
    stellar::Status why = stellar::Status::Ok;
    if (!stellar::stat(star, &si, snap, &why)) return status_from_stellar(why);
    out->kind = si.type == stellar::TYPE_CONSTELLATION ? Kind::Dir : Kind::File;
    out->size = si.type == stellar::TYPE_CONSTELLATION ? 0 : si.size_bytes;
    out->mtime_ms = si.mtime;
    out->links = si.nlink;
    out->read_only = read_only;
    out->dynamic = false;
    return Status::Ok;
}

class DiskRealm final : public Realm {
public:
    const char* name() const override { return "disk"; }
    const char* summary() const override { return "Stellar volume with snapshots and checksums"; }
    bool writable() const override { return true; }
    bool available() const override { return g_mounted; }
    Node root() override { return Node{stellar::ROOT_STAR, stellar::LIVE}; }

    Status find(Node dir, const char* name, Node* out) override {
        stellar::Status why = stellar::Status::Ok;
        u64 star = stellar::find(dir.id, name, stellar::LIVE, &why);
        if (star == stellar::INVALID_STAR) return status_from_stellar(why);
        *out = Node{star, stellar::LIVE};
        return Status::Ok;
    }

    Status info(Node node, Info* out) override { return fill_info(node.id, stellar::LIVE, out, false); }

    Status list(Node dir, ChildFn fn, void* ctx) override {
        ListCtx c{fn, ctx, stellar::LIVE};
        stellar::Status why = stellar::Status::Ok;
        stellar::list(dir.id, &list_bridge, &c, stellar::LIVE, &why);
        return status_from_stellar(why);
    }

    Status read_all(Node file, void* buf, u64 capacity, u64* size) override {
        stellar::Status why = stellar::Status::Ok;
        u64 n = stellar::read_file(file.id, buf, capacity, stellar::LIVE, &why);
        if (n == stellar::READ_ERROR) return status_from_stellar(why);
        *size = n;
        return Status::Ok;
    }

    Status write_all(Node file, const void* data, u64 size) override {
        stellar::Status why = stellar::Status::Ok;
        u64 r = stellar::write_file(file.id, data, size, &why);
        if (r == stellar::INVALID_STAR) return status_from_stellar(why);
        return Status::Ok;
    }

    Status create_file(Node dir, const char* name, Node* out) override {
        stellar::Status why = stellar::Status::Ok;
        u64 star = stellar::create_file(dir.id, name, nullptr, 0, &why);
        if (star == stellar::INVALID_STAR) return status_from_stellar(why);
        *out = Node{star, stellar::LIVE};
        return Status::Ok;
    }

    Status make_dir(Node dir, const char* name, Node* out) override {
        stellar::Status why = stellar::Status::Ok;
        u64 star = stellar::create_constellation(dir.id, name, &why);
        if (star == stellar::INVALID_STAR) return status_from_stellar(why);
        *out = Node{star, stellar::LIVE};
        return Status::Ok;
    }

    Status remove(Node dir, const char* name) override {
        stellar::Status why = stellar::Status::Ok;
        if (!stellar::unlink(dir.id, name, &why)) return status_from_stellar(why);
        return Status::Ok;
    }

    Status rename(Node from_dir, const char* from, Node to_dir, const char* to, bool replace) override {
        stellar::Status why = stellar::Status::Ok;
        u32 flags = replace ? 0 : stellar::RENAME_NOREPLACE;
        if (!stellar::rename(from_dir.id, from, to_dir.id, to, flags, &why)) return status_from_stellar(why);
        return Status::Ok;
    }

    u64 space_free() override { return g_mounted ? stellar::free_space_sectors() * 512 : 0; }
    u64 space_total() override { return g_mounted ? stellar::total_sectors() * 512 : 0; }
};

constexpr u64 INDEX_ID = 0;
constexpr u64 INDEX_SCOPE = 0;

bool is_index(Node n) { return n.id == INDEX_ID && n.scope == INDEX_SCOPE; }

bool parse_id(const char* s, u64* out) {
    if (!*s || (s[0] == '0' && s[1])) return false;
    u64 v = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return false;
        if (v > (~0ull - 9) / 10) return false;
        v = v * 10 + static_cast<u64>(*s - '0');
    }
    if (v == 0) return false;
    *out = v;
    return true;
}

u32 format_id(u64 v, char* out) {
    char t[24];
    u32 n = 0;
    while (v) { t[n++] = static_cast<char>('0' + v % 10); v /= 10; }
    for (u32 i = 0; i < n; ++i) out[i] = t[n - 1 - i];
    out[n] = 0;
    return n;
}

class SnapRealm final : public Realm {
public:
    const char* name() const override { return "snap"; }
    const char* summary() const override { return "read-only views of Stellar snapshots, one directory per snapshot"; }
    bool writable() const override { return false; }
    bool available() const override { return g_mounted; }
    Node root() override { return Node{INDEX_ID, INDEX_SCOPE}; }

    Status find(Node dir, const char* name, Node* out) override {
        if (is_index(dir)) {
            u64 id;
            if (!parse_id(name, &id)) return Status::NotFound;
            stellar::Status why = stellar::Status::Ok;
            if (stellar::snapshot_scope(id, &why) == stellar::INVALID_STAR) {
                return why == stellar::Status::NoSuchSnapshot ? Status::NotFound : status_from_stellar(why);
            }
            *out = Node{stellar::ROOT_STAR, id};
            return Status::Ok;
        }
        stellar::Status why = stellar::Status::Ok;
        u64 star = stellar::find(dir.id, name, dir.scope, &why);
        if (star == stellar::INVALID_STAR) return status_from_stellar(why);
        *out = Node{star, dir.scope};
        return Status::Ok;
    }

    Status info(Node node, Info* out) override {
        if (is_index(node)) {
            out->kind = Kind::Dir;
            out->size = 0;
            out->mtime_ms = 0;
            out->links = 1;
            out->read_only = true;
            out->dynamic = false;
            return Status::Ok;
        }
        return fill_info(node.id, node.scope, out, true);
    }

    Status list(Node dir, ChildFn fn, void* ctx) override {
        if (is_index(dir)) {
            u64 ids[stellar::SNAPSHOT_MAX];
            u32 n = stellar::snapshot_ids(ids, stellar::SNAPSHOT_MAX);
            for (u32 i = 0; i < n; ++i) {
                char name[24];
                format_id(ids[i], name);
                fn(name, Kind::Dir, Node{stellar::ROOT_STAR, ids[i]}, ctx);
            }
            return Status::Ok;
        }
        ListCtx c{fn, ctx, dir.scope};
        stellar::Status why = stellar::Status::Ok;
        stellar::list(dir.id, &list_bridge, &c, dir.scope, &why);
        return status_from_stellar(why);
    }

    Status read_all(Node file, void* buf, u64 capacity, u64* size) override {
        if (is_index(file)) return Status::IsADirectory;
        stellar::Status why = stellar::Status::Ok;
        u64 n = stellar::read_file(file.id, buf, capacity, file.scope, &why);
        if (n == stellar::READ_ERROR) return status_from_stellar(why);
        *size = n;
        return Status::Ok;
    }

    Status write_all(Node, const void*, u64) override { return Status::ReadOnly; }
    Status create_file(Node, const char*, Node*) override { return Status::ReadOnly; }
    Status make_dir(Node, const char*, Node*) override { return Status::ReadOnly; }
    Status remove(Node, const char*) override { return Status::ReadOnly; }
    Status rename(Node, const char*, Node, const char*, bool) override { return Status::ReadOnly; }
};

constinit DiskRealm g_disk;
constinit SnapRealm g_snap;

}

Status status_from_stellar(stellar::Status s) {
    switch (s) {
        case stellar::Status::Ok: return Status::Ok;
        case stellar::Status::NotMounted:
        case stellar::Status::NotFormatted: return Status::Unavailable;
        case stellar::Status::InvalidArgument: return Status::InvalidPath;
        case stellar::Status::InvalidName: return Status::InvalidName;
        case stellar::Status::NotFound:
        case stellar::Status::NoSuchSnapshot: return Status::NotFound;
        case stellar::Status::Exists: return Status::Exists;
        case stellar::Status::NotADirectory: return Status::NotADirectory;
        case stellar::Status::IsADirectory: return Status::IsADirectory;
        case stellar::Status::NotEmpty: return Status::NotEmpty;
        case stellar::Status::NoSpace:
        case stellar::Status::TooManySnapshots: return Status::NoSpace;
        case stellar::Status::NoMemory: return Status::NoMemory;
        case stellar::Status::Io: return Status::Io;
        case stellar::Status::Checksum:
        case stellar::Status::Corrupt: return Status::Corrupt;
        case stellar::Status::Busy: return Status::Busy;
        case stellar::Status::Unsupported:
        case stellar::Status::WouldCycle: return Status::Unsupported;
        case stellar::Status::Internal: return Status::Io;
    }
    return Status::Io;
}

Realm* disk_realm() { return &g_disk; }
Realm* snap_realm() { return &g_snap; }

void disk_set_mounted(bool mounted) { g_mounted = mounted; }
bool disk_is_mounted() { return g_mounted; }
void disk_set_confirmed_only(bool confirmed_only) { g_confirmed_only = confirmed_only; }
bool disk_is_confirmed_only() { return g_confirmed_only; }

Status disk_snapshot(u64* id) {
    if (!g_mounted) return Status::Unavailable;
    stellar::Status why = stellar::Status::Ok;
    u64 r = stellar::snapshot(&why);
    if (r == stellar::INVALID_STAR) return status_from_stellar(why);
    *id = r;
    return Status::Ok;
}

Status disk_snapshot_delete(u64 id) {
    if (!g_mounted) return Status::Unavailable;
    stellar::Status why = stellar::Status::Ok;
    if (!stellar::delete_snapshot(id, &why)) return status_from_stellar(why);
    return Status::Ok;
}

}
