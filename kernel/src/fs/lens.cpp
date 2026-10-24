#include <cosmos/lens.hpp>

namespace lens {

namespace {

bool g_sealed;

void copy_label(char* dst, const char* src, u32 n) {
    if (n > nebula::NAME_BYTES) n = nebula::NAME_BYTES;
    for (u32 i = 0; i < n; ++i) dst[i] = src[i];
    dst[n] = 0;
}

u32 text_len(const char* s) {
    u32 n = 0;
    while (s[n]) ++n;
    return n;
}

bool is_dotdot(const char* p, u32 n) { return n == 2 && p[0] == '.' && p[1] == '.'; }

}

const char* kind_name(NodeKind k) {
    switch (k) {
        case NodeKind::Blob: return "blob";
        case NodeKind::Folder: return "folder";
        case NodeKind::Probe: return "probe";
        case NodeKind::Stream: return "stream";
    }
    return "?";
}

Status from_nebula(nebula::Status s) {
    switch (s) {
        case nebula::Status::Ok: return Status::Ok;
        case nebula::Status::NotFound: return Status::NotFound;
        case nebula::Status::Exists: return Status::Exists;
        case nebula::Status::NotADirectory: return Status::NotADirectory;
        case nebula::Status::IsADirectory: return Status::IsADirectory;
        case nebula::Status::NotEmpty: return Status::NotEmpty;
        case nebula::Status::NoSpace: return Status::NoSpace;
        case nebula::Status::NoMemory: return Status::NoMemory;
        case nebula::Status::Io: return Status::Io;
        case nebula::Status::InvalidPath: return Status::InvalidPath;
        case nebula::Status::InvalidName: return Status::InvalidName;
        case nebula::Status::ReadOnly: return Status::ReadOnly;
        case nebula::Status::Busy: return Status::Busy;
        case nebula::Status::TooLarge: return Status::TooLarge;
        case nebula::Status::NoRealm:
        case nebula::Status::Unavailable: return Status::Unavailable;
        case nebula::Status::BadHandle: return Status::BadLens;
        case nebula::Status::CrossRealm: return Status::CrossRealm;
        case nebula::Status::Unsupported: return Status::Unsupported;
        case nebula::Status::Corrupt: return Status::Corrupt;
        case nebula::Status::Denied: return Status::Denied;
    }
    return Status::Io;
}

NodeKind kind_from_info(const nebula::Info& inf) {
    if (inf.kind == nebula::Kind::Dir) return NodeKind::Folder;
    return inf.dynamic ? NodeKind::Probe : NodeKind::Blob;
}

Result<Slot*> slot_of(Domain& d, Lens l) {
    u32 index = static_cast<u32>(l.raw & 0xFFFFFFFFull);
    u32 gen = static_cast<u32>(l.raw >> 32);
    if (l.raw == 0 || index == 0 || index > MAX_LENSES) return Result<Slot*>::fail(Status::BadLens);
    Slot& s = d.slots[index - 1];
    if (!s.used || s.generation != gen) return Result<Slot*>::fail(Status::StaleLens);
    return Result<Slot*>::success(&s);
}

Result<Lens> alloc_slot(Domain& d, nebula::Realm* realm, nebula::Node node, nebula::Node root, Rights rights,
                        bool read_only, u32 depth, const char* label, Stream* stream) {
    u32 index = MAX_LENSES;
    for (u32 i = 0; i < MAX_LENSES; ++i)
        if (!d.slots[i].used && !d.slots[i].retired) { index = i; break; }
    if (index == MAX_LENSES) return Result<Lens>::fail(Status::TableFull);

    NodeKind kind = NodeKind::Stream;
    if (!stream) {
        nebula::Info inf{};
        Status st = from_nebula(realm->info(node, &inf));
        kind = NodeKind::Folder;
        if (st == Status::Ok) kind = kind_from_info(inf);
        else if (!realm->available()) kind = NodeKind::Folder;
        else return Result<Lens>::fail(st);
    }

    Slot& s = d.slots[index];
    u32 gen = s.generation + 1;
    s = Slot{};
    s.used = true;
    s.generation = gen;
    s.kind = kind;
    s.read_only = read_only;
    s.depth = static_cast<u8>(depth);
    s.rights = rights;
    s.realm = realm;
    s.node = node;
    s.root = root;
    s.stream = stream;
    if (stream) stream->retain();
    copy_label(s.label, label, text_len(label));
    ++d.live;
    return Result<Lens>::success(Lens((static_cast<u64>(gen) << 32) | (index + 1)));
}

Status parse_relative(const char* text, RelPath* out) {
    out->count = 0;
    if (!text || !*text) return Status::InvalidPath;
    u32 len = text_len(text);
    if (len >= nebula::path::TEXT_MAX) return Status::InvalidPath;
    if (text[0] == '/') return Status::Escape;
    for (u32 i = 0; i < len && text[i] != '/'; ++i)
        if (text[i] == ':') return Status::Escape;

    u32 i = 0;
    while (i < len) {
        while (i < len && text[i] == '/') ++i;
        if (i >= len) break;
        u32 j = i;
        while (j < len && text[j] != '/') ++j;
        u32 n = j - i;
        if (n == 1 && text[i] == '.') { i = j; continue; }
        if (is_dotdot(text + i, n)) return Status::Escape;
        if (n > nebula::path::NAME_MAX) return Status::InvalidName;
        if (!nebula::path::name_ok(text + i, n)) return Status::InvalidName;
        if (out->count >= nebula::path::MAX_COMPONENTS) return Status::InvalidPath;
        out->comps[out->count++] = Comp{text + i, n};
        i = j;
    }
    return Status::Ok;
}

Status walk(nebula::Realm* realm, nebula::Node from, const RelPath& rp, u32 count, nebula::Node* out) {
    nebula::Node cur = from;
    for (u32 i = 0; i < count && i < rp.count; ++i) {
        nebula::Info inf;
        Status st = from_nebula(realm->info(cur, &inf));
        if (st != Status::Ok) return st;
        if (inf.kind != nebula::Kind::Dir) return Status::NotADirectory;
        char name[nebula::NAME_BYTES + 1];
        copy_label(name, rp.comps[i].p, rp.comps[i].n);
        nebula::Node next;
        st = from_nebula(realm->find(cur, name, &next));
        if (st != Status::Ok) return st;
        cur = next;
    }
    *out = cur;
    return Status::Ok;
}

void seal() { g_sealed = true; }
bool sealed() { return g_sealed; }
void test_unseal() { g_sealed = false; }

Result<Lens> mint(Domain& d, const char* text, Rights rights, bool read_only) {
    if (g_sealed) return Result<Lens>::fail(Status::Denied);
    if (!text || !*text) return Result<Lens>::fail(Status::InvalidPath);

    u32 len = text_len(text);
    u32 start = 0;
    for (u32 i = 0; i < len && text[i] != '/'; ++i)
        if (text[i] == ':') { start = i + 1; break; }
    if (start == 0) return Result<Lens>::fail(Status::InvalidPath);
    for (u32 i = start; i < len;) {
        while (i < len && text[i] == '/') ++i;
        u32 j = i;
        while (j < len && text[j] != '/') ++j;
        if (is_dotdot(text + i, j - i)) return Result<Lens>::fail(Status::Escape);
        i = j;
    }

    nebula::path::Path here;
    nebula::path::clear(here);
    nebula::path::Path p;
    nebula::path::Error e = nebula::path::resolve(here, text, p);
    if (e == nebula::path::Error::BadName || e == nebula::path::Error::NameTooLong)
        return Result<Lens>::fail(Status::InvalidName);
    if (e != nebula::path::Error::Ok) return Result<Lens>::fail(Status::InvalidPath);

    nebula::Realm* realm = nebula::find_realm(p.realm);
    if (!realm) return Result<Lens>::fail(Status::Unavailable);
    if (!realm->available() && p.count > 0) return Result<Lens>::fail(Status::Unavailable);

    nebula::Node node = realm->root();
    for (u32 i = 0; i < p.count; ++i) {
        nebula::Info inf;
        Status st = from_nebula(realm->info(node, &inf));
        if (st != Status::Ok) return Result<Lens>::fail(st);
        if (inf.kind != nebula::Kind::Dir) return Result<Lens>::fail(Status::NotADirectory);
        nebula::Node next;
        st = from_nebula(realm->find(node, p.comps[i], &next));
        if (st != Status::Ok) return Result<Lens>::fail(st);
        node = next;
    }
    const char* label = p.count ? p.comps[p.count - 1] : p.realm;
    return alloc_slot(d, realm, node, node, rights, read_only, 0, label);
}

Result<Lens> open_at(Domain& d, Lens dir, const char* rel_path, Rights rights) {
    Result<Slot*> sr = slot_of(d, dir);
    if (!sr.ok()) return Result<Lens>::fail(sr.status());
    Slot* parent = sr.value();
    if (parent->kind != NodeKind::Folder) return Result<Lens>::fail(Status::NotADirectory);

    if (parent->rights.empty() || !rights.subset_of(parent->rights)) return Result<Lens>::fail(Status::Denied);

    RelPath rp;
    Status st = parse_relative(rel_path, &rp);
    if (st != Status::Ok) return Result<Lens>::fail(st);
    if (parent->depth + rp.count > MAX_DEPTH) return Result<Lens>::fail(Status::InvalidPath);
    if (!parent->realm->available()) return Result<Lens>::fail(Status::Unavailable);

    nebula::Node node;
    st = walk(parent->realm, parent->node, rp, rp.count, &node);
    if (st != Status::Ok) return Result<Lens>::fail(st);

    char label[nebula::NAME_BYTES + 1];
    if (rp.count) copy_label(label, rp.comps[rp.count - 1].p, rp.comps[rp.count - 1].n);
    else copy_label(label, parent->label, text_len(parent->label));

    nebula::Realm* realm = parent->realm;
    nebula::Node root = parent->root;
    bool ro = parent->read_only;
    u32 depth = parent->depth + rp.count;
    return alloc_slot(d, realm, node, root, rights, ro, depth, label);
}

Result<Lens> narrow(Domain& d, Lens src, Rights keep, bool make_read_only) {
    Result<Slot*> sr = slot_of(d, src);
    if (!sr.ok()) return Result<Lens>::fail(sr.status());
    Slot* s = sr.value();
    Rights rights = s->rights & keep;
    nebula::Realm* realm = s->realm;
    nebula::Node node = s->node, root = s->root;
    bool ro = s->read_only || make_read_only;
    u32 depth = s->depth;
    Stream* stream = s->stream;
    char label[nebula::NAME_BYTES + 1];
    copy_label(label, s->label, text_len(s->label));
    return alloc_slot(d, realm, node, root, rights, ro, depth, label, stream);
}

Result<Lens> delegate(Domain& from, Lens src, Domain& to, Rights rights) {
    Result<Slot*> sr = slot_of(from, src);
    if (!sr.ok()) return Result<Lens>::fail(sr.status());
    Slot* s = sr.value();
    if (!s->rights.has(Right::Delegate)) return Result<Lens>::fail(Status::Denied);
    if (!rights.subset_of(s->rights)) return Result<Lens>::fail(Status::Denied);
    nebula::Realm* realm = s->realm;
    nebula::Node node = s->node;
    bool ro = s->read_only;
    char label[nebula::NAME_BYTES + 1];
    copy_label(label, s->label, text_len(s->label));

    return alloc_slot(to, realm, node, node, rights, ro, 0, label, s->stream);
}

Done close(Domain& d, Lens l) {
    Result<Slot*> sr = slot_of(d, l);
    if (!sr.ok()) return Done::fail(sr.status());
    Slot* s = sr.value();
    Status st = Status::Ok;
    if (s->buffer.used) st = from_nebula(nebula::file_close(s->buffer));
    Stream* stream = s->stream;
    u32 gen = s->generation;
    *s = Slot{};
    s->generation = gen;
    s->retired = gen == 0xFFFFFFFFu;
    if (d.live) --d.live;
    if (stream) stream->release();
    return st == Status::Ok ? Done::success() : Done::fail(st);
}

void close_all(Domain& d) {
    for (u32 i = 0; i < MAX_LENSES; ++i) {
        if (!d.slots[i].used) continue;
        Lens l((static_cast<u64>(d.slots[i].generation) << 32) | (i + 1));
        (void)close(d, l);
    }
}

Result<Rights> rights_of(Domain& d, Lens l) {
    Result<Slot*> sr = slot_of(d, l);
    if (!sr.ok()) return Result<Rights>::fail(sr.status());
    return Result<Rights>::success(sr.value()->rights);
}

Result<NodeKind> kind_of(Domain& d, Lens l) {
    Result<Slot*> sr = slot_of(d, l);
    if (!sr.ok()) return Result<NodeKind>::fail(sr.status());
    return Result<NodeKind>::success(sr.value()->kind);
}

Result<bool> read_only_of(Domain& d, Lens l) {
    Result<Slot*> sr = slot_of(d, l);
    if (!sr.ok()) return Result<bool>::fail(sr.status());
    return Result<bool>::success(sr.value()->read_only);
}

Result<u32> depth_of(Domain& d, Lens l) {
    Result<Slot*> sr = slot_of(d, l);
    if (!sr.ok()) return Result<u32>::fail(sr.status());
    return Result<u32>::success(sr.value()->depth);
}

Result<nebula::Info> stat(Domain& d, Lens l) {
    Result<Slot*> sr = slot_of(d, l);
    if (!sr.ok()) return Result<nebula::Info>::fail(sr.status());
    Slot* s = sr.value();
    if (s->rights.empty()) return Result<nebula::Info>::fail(Status::Denied);
    if (s->kind == NodeKind::Stream) return Result<nebula::Info>::fail(Status::Unsupported);
    if (!s->realm->available()) return Result<nebula::Info>::fail(Status::Unavailable);
    nebula::Info inf{};
    Status st = from_nebula(s->realm->info(s->node, &inf));
    if (st != Status::Ok) return Result<nebula::Info>::fail(st);
    return Result<nebula::Info>::success(inf);
}

Result<u32> label_of(Domain& d, Lens l, char* out, u32 cap) {
    Result<Slot*> sr = slot_of(d, l);
    if (!sr.ok()) return Result<u32>::fail(sr.status());
    const char* label = sr.value()->label;
    u32 n = text_len(label);
    if (cap) {
        u32 c = n < cap - 1 ? n : cap - 1;
        for (u32 i = 0; i < c; ++i) out[i] = label[i];
        out[c] = 0;
    }
    return Result<u32>::success(n);
}

bool is_live(Domain& d, Lens l) { return slot_of(d, l).ok(); }

u32 live_count(const Domain& d) { return d.live; }

}
