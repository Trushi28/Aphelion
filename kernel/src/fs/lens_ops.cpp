#include <cosmos/lens.hpp>
#include <cosmos/kheap.hpp>

namespace lens {

namespace {

using nebula::Info;
using nebula::Kind;
using nebula::Node;
using nebula::Realm;

bool same_node(Node a, Node b) { return a.id == b.id && a.scope == b.scope; }

void copy_name(char* dst, const char* src, u32 n) {
    if (n > nebula::NAME_BYTES) n = nebula::NAME_BYTES;
    for (u32 i = 0; i < n; ++i) dst[i] = src[i];
    dst[n] = 0;
}

struct Leaf {
    char name[nebula::NAME_BYTES + 1];
};

Status folder_slot(Domain& d, Lens l, Slot** out) {
    Result<Slot*> sr = slot_of(d, l);
    if (!sr.ok()) return sr.status();
    Slot* s = sr.value();
    if (s->kind != NodeKind::Folder) return Status::NotADirectory;
    *out = s;
    return Status::Ok;
}

Status check_rights(const Slot* s, Rights need) { return s->rights.has_all(need) ? Status::Ok : Status::Denied; }
Status check_any(const Slot* s, Rights any) { return s->rights.has_any(any) ? Status::Ok : Status::Denied; }

Status check_mutable(const Slot* s) {
    if (s->read_only) return Status::ReadOnly;
    if (!s->realm->available()) return Status::Unavailable;
    if (!s->realm->writable()) return Status::ReadOnly;
    return Status::Ok;
}

Status check_available(const Slot* s) { return s->realm->available() ? Status::Ok : Status::Unavailable; }

Status locate(const Slot* dir, const RelPath& rp, Node* parent, Leaf* leaf) {
    if (rp.count == 0) return Status::InvalidPath;
    if (dir->depth + rp.count > MAX_DEPTH) return Status::InvalidPath;
    Node p;
    Status st = walk(dir->realm, dir->node, rp, rp.count - 1, &p);
    if (st != Status::Ok) return st;
    Info inf;
    st = from_nebula(dir->realm->info(p, &inf));
    if (st != Status::Ok) return st;
    if (inf.kind != Kind::Dir) return Status::NotADirectory;
    copy_name(leaf->name, rp.comps[rp.count - 1].p, rp.comps[rp.count - 1].n);
    *parent = p;
    return Status::Ok;
}

bool passes_through(Realm* realm, Node from, const RelPath& rp, u32 count, Node target) {
    Node cur = from;
    if (same_node(cur, target)) return true;
    for (u32 i = 0; i < count; ++i) {
        char name[nebula::NAME_BYTES + 1];
        copy_name(name, rp.comps[i].p, rp.comps[i].n);
        Node next;
        if (realm->find(cur, name, &next) != nebula::Status::Ok) return false;
        cur = next;
        if (same_node(cur, target)) return true;
    }
    return false;
}

Status ensure_buffer(Slot* s, bool for_write) {
    if (s->buffer.used) {
        bool writer = (s->buffer.flags & nebula::OPEN_WRITE) != 0;
        if (!for_write || writer) return Status::Ok;
        nebula::file_close(s->buffer);
    }
    u32 flags = nebula::OPEN_READ | (for_write ? nebula::OPEN_WRITE : 0u);
    return from_nebula(nebula::open_node(s->buffer, s->realm, s->node, flags));
}

Status data_slot(Domain& d, Lens l, Slot** out, bool allow_probe) {
    Result<Slot*> sr = slot_of(d, l);
    if (!sr.ok()) return sr.status();
    Slot* s = sr.value();
    if (s->kind == NodeKind::Blob || (allow_probe && s->kind == NodeKind::Probe)) { *out = s; return Status::Ok; }
    return s->kind == NodeKind::Folder ? Status::IsADirectory : Status::NotAFile;
}

}

Result<usize> read(Domain& d, Lens l, u64 offset, Bytes dst) {
    Result<Slot*> any = slot_of(d, l);
    if (any.ok() && any.value()->kind == NodeKind::Stream) {
        if (!any.value()->rights.has(Right::Read)) return Result<usize>::fail(Status::Denied);
        return any.value()->stream->read(dst);
    }
    Slot* s;
    Status st = data_slot(d, l, &s, true);
    if (st == Status::Ok) st = check_rights(s, Right::Read);
    if (st == Status::Ok) st = check_available(s);
    if (st != Status::Ok) return Result<usize>::fail(st);

    if (s->kind == NodeKind::Probe && offset == 0 && s->buffer.used) nebula::file_close(s->buffer);
    st = ensure_buffer(s, false);
    if (st != Status::Ok) return Result<usize>::fail(st);
    u64 got = 0;
    st = from_nebula(nebula::file_read(s->buffer, offset, dst.ptr, dst.len, &got));
    if (st == Status::Busy && s->buffer.ranged) {
        (void)nebula::file_close(s->buffer);
        st = ensure_buffer(s, false);
        if (st == Status::Ok) st = from_nebula(nebula::file_read(s->buffer, offset, dst.ptr, dst.len, &got));
    }
    if (st != Status::Ok) return Result<usize>::fail(st);
    return Result<usize>::success(static_cast<usize>(got));
}

Result<usize> write(Domain& d, Lens l, u64 offset, ConstBytes src) {
    Result<Slot*> any = slot_of(d, l);
    if (any.ok() && any.value()->kind == NodeKind::Stream) {
        Slot* t = any.value();
        if (!t->rights.has_any(Right::Write | Right::Append)) return Result<usize>::fail(Status::Denied);
        if (t->read_only) return Result<usize>::fail(Status::ReadOnly);
        return t->stream->write(src);
    }
    Slot* s;
    Status st = data_slot(d, l, &s, true);
    if (st == Status::Ok) st = check_rights(s, Right::Write);
    if (st == Status::Ok) st = check_mutable(s);
    if (st != Status::Ok) return Result<usize>::fail(st);
    st = ensure_buffer(s, true);
    if (st != Status::Ok) return Result<usize>::fail(st);
    st = from_nebula(nebula::file_write(s->buffer, offset, src.ptr, src.len));
    if (st != Status::Ok) return Result<usize>::fail(st);
    return Result<usize>::success(src.len);
}

Done append(Domain& d, Lens l, ConstBytes src) {
    Result<Slot*> any = slot_of(d, l);
    if (any.ok() && any.value()->kind == NodeKind::Stream) {
        Result<usize> w = write(d, l, 0, src);
        return w.ok() ? Done::success() : Done::fail(w.status());
    }
    Slot* s;
    Status st = data_slot(d, l, &s, true);
    if (st == Status::Ok) st = check_any(s, Right::Append | Right::Write);
    if (st == Status::Ok) st = check_mutable(s);
    if (st != Status::Ok) return Done::fail(st);
    st = ensure_buffer(s, true);
    if (st != Status::Ok) return Done::fail(st);
    st = from_nebula(nebula::file_append(s->buffer, src.ptr, src.len));
    return st == Status::Ok ? Done::success() : Done::fail(st);
}

Done resize(Domain& d, Lens l, u64 new_size) {
    Result<Slot*> any = slot_of(d, l);
    if (any.ok() && any.value()->kind == NodeKind::Stream) return Done::fail(Status::Unsupported);
    Slot* s;
    Status st = data_slot(d, l, &s, true);
    if (st == Status::Ok) st = check_rights(s, Right::Write);
    if (st == Status::Ok) st = check_mutable(s);
    if (st != Status::Ok) return Done::fail(st);
    st = ensure_buffer(s, true);
    if (st != Status::Ok) return Done::fail(st);
    st = from_nebula(nebula::file_resize(s->buffer, new_size));
    return st == Status::Ok ? Done::success() : Done::fail(st);
}

Done sync(Domain& d, Lens l) {
    Result<Slot*> any = slot_of(d, l);
    if (any.ok() && any.value()->kind == NodeKind::Stream) {
        return any.value()->rights.has_any(Right::Write | Right::Append) ? Done::success() : Done::fail(Status::Denied);
    }
    Slot* s;
    Status st = data_slot(d, l, &s, true);
    if (st == Status::Ok) st = check_any(s, Right::Write | Right::Append);
    if (st != Status::Ok) return Done::fail(st);
    if (!s->buffer.used) return Done::success();
    st = from_nebula(nebula::file_sync(s->buffer));
    return st == Status::Ok ? Done::success() : Done::fail(st);
}

Result<u64> size(Domain& d, Lens l) {
    Result<Slot*> any = slot_of(d, l);
    if (any.ok() && any.value()->kind == NodeKind::Stream) return Result<u64>::fail(Status::Unsupported);
    Slot* s;
    Status st = data_slot(d, l, &s, true);
    if (st == Status::Ok && s->rights.empty()) st = Status::Denied;
    if (st != Status::Ok) return Result<u64>::fail(st);
    if (s->buffer.used && (s->buffer.flags & nebula::OPEN_WRITE)) return Result<u64>::success(s->buffer.size);
    Result<nebula::Info> inf = stat(d, l);
    if (!inf.ok()) return Result<u64>::fail(inf.status());
    return Result<u64>::success(inf.value().size);
}

Result<EnumSummary> enumerate(Domain& d, Lens dir, VisitFn fn, void* ctx, bool with_info) {
    Slot* s;
    Status st = folder_slot(d, dir, &s);
    if (st == Status::Ok) st = check_rights(s, Right::Enumerate);
    if (st == Status::Ok) st = check_available(s);
    if (st != Status::Ok) return Result<EnumSummary>::fail(st);

    nebula::Listing listing;
    st = from_nebula(nebula::list_node(s->realm, s->node, &listing, with_info, false));
    if (st != Status::Ok) return Result<EnumSummary>::fail(st);
    EnumSummary sum{};
    sum.truncated = listing.truncated;
    for (u32 i = 0; i < listing.count; ++i) {
        ++sum.visited;
        if (!fn(ctx, listing.items[i])) break;
    }
    nebula::release(listing);
    return Result<EnumSummary>::success(sum);
}

Done collect(Domain& d, Lens dir, nebula::Listing* out, bool with_info, bool sorted) {
    *out = nebula::Listing{};
    Slot* s;
    Status st = folder_slot(d, dir, &s);
    if (st == Status::Ok) st = check_rights(s, Right::Enumerate);
    if (st == Status::Ok) st = check_available(s);
    if (st != Status::Ok) return Done::fail(st);
    st = from_nebula(nebula::list_node(s->realm, s->node, out, with_info, sorted));
    return st == Status::Ok ? Done::success() : Done::fail(st);
}

Result<nebula::Blob> slurp(Domain& d, Lens l) {
    Slot* s;
    Status st = data_slot(d, l, &s, true);
    if (st == Status::Ok) st = check_rights(s, Right::Read);
    if (st == Status::Ok) st = check_available(s);
    if (st != Status::Ok) return Result<nebula::Blob>::fail(st);
    nebula::Blob blob{};
    if (s->buffer.used && (s->buffer.flags & nebula::OPEN_WRITE)) {
        if (s->buffer.size) {
            blob.data = static_cast<u8*>(kheap::alloc(static_cast<usize>(s->buffer.size)));
            if (!blob.data) return Result<nebula::Blob>::fail(Status::NoMemory);
            mem::copy(blob.data, s->buffer.data, static_cast<usize>(s->buffer.size));
            blob.size = s->buffer.size;
        }
        return Result<nebula::Blob>::success(blob);
    }
    st = from_nebula(nebula::read_node(s->realm, s->node, &blob));
    if (st != Status::Ok) return Result<nebula::Blob>::fail(st);
    return Result<nebula::Blob>::success(blob);
}

Result<Lens> create(Domain& d, Lens dir, const char* rel, Rights rights) {
    Slot* s;
    Status st = folder_slot(d, dir, &s);
    if (st == Status::Ok) st = check_rights(s, Right::Create);
    if (st == Status::Ok && !rights.subset_of(s->rights)) st = Status::Denied;
    if (st == Status::Ok) st = check_mutable(s);
    if (st != Status::Ok) return Result<Lens>::fail(st);
    RelPath rp;
    st = parse_relative(rel, &rp);
    if (st != Status::Ok) return Result<Lens>::fail(st);
    if (d.live >= MAX_LENSES) return Result<Lens>::fail(Status::TableFull);

    Node parent;
    Leaf leaf;
    st = locate(s, rp, &parent, &leaf);
    if (st != Status::Ok) return Result<Lens>::fail(st);
    Node existing;
    nebula::Status f = s->realm->find(parent, leaf.name, &existing);
    if (f == nebula::Status::Ok) return Result<Lens>::fail(Status::Exists);
    if (f != nebula::Status::NotFound) return Result<Lens>::fail(from_nebula(f));

    Node node;
    st = from_nebula(s->realm->create_file(parent, leaf.name, &node));
    if (st != Status::Ok) return Result<Lens>::fail(st);
    Result<Lens> made = alloc_slot(d, s->realm, node, s->root, rights, s->read_only, s->depth + rp.count, leaf.name);
    if (!made.ok()) s->realm->remove(parent, leaf.name);
    return made;
}

Done mkdir(Domain& d, Lens dir, const char* rel, bool parents) {
    Slot* s;
    Status st = folder_slot(d, dir, &s);
    if (st == Status::Ok) st = check_rights(s, Right::Create);
    if (st == Status::Ok) st = check_mutable(s);
    if (st != Status::Ok) return Done::fail(st);
    RelPath rp;
    st = parse_relative(rel, &rp);
    if (st != Status::Ok) return Done::fail(st);
    if (rp.count == 0) return Done::fail(Status::InvalidPath);
    if (s->depth + rp.count > MAX_DEPTH) return Done::fail(Status::InvalidPath);

    if (!parents) {
        Node parent;
        Leaf leaf;
        st = locate(s, rp, &parent, &leaf);
        if (st != Status::Ok) return Done::fail(st);
        Node existing;
        nebula::Status f = s->realm->find(parent, leaf.name, &existing);
        if (f == nebula::Status::Ok) return Done::fail(Status::Exists);
        if (f != nebula::Status::NotFound) return Done::fail(from_nebula(f));
        Node made;
        st = from_nebula(s->realm->make_dir(parent, leaf.name, &made));
        return st == Status::Ok ? Done::success() : Done::fail(st);
    }

    Node cur = s->node;
    for (u32 i = 0; i < rp.count; ++i) {
        Info inf;
        st = from_nebula(s->realm->info(cur, &inf));
        if (st != Status::Ok) return Done::fail(st);
        if (inf.kind != Kind::Dir) return Done::fail(Status::NotADirectory);
        char name[nebula::NAME_BYTES + 1];
        copy_name(name, rp.comps[i].p, rp.comps[i].n);
        Node next;
        nebula::Status f = s->realm->find(cur, name, &next);
        if (f == nebula::Status::NotFound) f = s->realm->make_dir(cur, name, &next);
        if (f != nebula::Status::Ok) return Done::fail(from_nebula(f));
        cur = next;
    }
    Info last;
    st = from_nebula(s->realm->info(cur, &last));
    if (st != Status::Ok) return Done::fail(st);
    return last.kind == Kind::Dir ? Done::success() : Done::fail(Status::NotADirectory);
}

Done unlink(Domain& d, Lens dir, const char* rel) {
    Slot* s;
    Status st = folder_slot(d, dir, &s);
    if (st == Status::Ok) st = check_rights(s, Right::Unlink);
    if (st == Status::Ok) st = check_mutable(s);
    if (st != Status::Ok) return Done::fail(st);
    RelPath rp;
    st = parse_relative(rel, &rp);
    if (st != Status::Ok) return Done::fail(st);
    Node parent;
    Leaf leaf;
    st = locate(s, rp, &parent, &leaf);
    if (st != Status::Ok) return Done::fail(st);
    Node node;
    st = from_nebula(s->realm->find(parent, leaf.name, &node));
    if (st != Status::Ok) return Done::fail(st);
    if (nebula::node_busy(s->realm, node)) return Done::fail(Status::Busy);
    st = from_nebula(s->realm->remove(parent, leaf.name));
    return st == Status::Ok ? Done::success() : Done::fail(st);
}

namespace {

Status remove_below(Realm* realm, Node dir, u32 depth, u32* removed) {
    if (depth > nebula::MAX_TREE_DEPTH) return Status::Unsupported;
    nebula::Listing l;
    Status st = from_nebula(nebula::list_node(realm, dir, &l, false, false));
    if (st != Status::Ok) return st;
    for (u32 i = 0; i < l.count && st == Status::Ok; ++i) {
        const nebula::Entry& e = l.items[i];
        Node child;
        st = from_nebula(realm->find(dir, e.name, &child));
        if (st != Status::Ok) break;
        if (e.kind == Kind::Dir) st = remove_below(realm, child, depth + 1, removed);
        else if (nebula::node_busy(realm, child)) st = Status::Busy;
        if (st != Status::Ok) break;
        st = from_nebula(realm->remove(dir, e.name));
        if (st == Status::Ok && removed) ++*removed;
    }
    nebula::release(l);
    return st;
}

}

Done remove_tree(Domain& d, Lens dir, const char* rel, u32* removed) {
    if (removed) *removed = 0;
    Slot* s;
    Status st = folder_slot(d, dir, &s);
    if (st == Status::Ok) st = check_rights(s, Right::Unlink);
    if (st == Status::Ok) st = check_mutable(s);
    if (st != Status::Ok) return Done::fail(st);
    RelPath rp;
    st = parse_relative(rel, &rp);
    if (st != Status::Ok) return Done::fail(st);
    Node parent;
    Leaf leaf;
    st = locate(s, rp, &parent, &leaf);
    if (st != Status::Ok) return Done::fail(st);
    Node node;
    st = from_nebula(s->realm->find(parent, leaf.name, &node));
    if (st != Status::Ok) return Done::fail(st);
    Info inf;
    st = from_nebula(s->realm->info(node, &inf));
    if (st != Status::Ok) return Done::fail(st);
    if (inf.kind == Kind::Dir) {
        st = remove_below(s->realm, node, 0, removed);
        if (st != Status::Ok) return Done::fail(st);
    } else if (nebula::node_busy(s->realm, node)) {
        return Done::fail(Status::Busy);
    }
    st = from_nebula(s->realm->remove(parent, leaf.name));
    if (st == Status::Ok && removed) ++*removed;
    return st == Status::Ok ? Done::success() : Done::fail(st);
}

Done rename(Domain& d, Lens from_dir, const char* from, Lens to_dir, const char* to, bool replace) {
    Slot* a;
    Slot* b;
    Status st = folder_slot(d, from_dir, &a);
    if (st == Status::Ok) st = folder_slot(d, to_dir, &b);
    if (st == Status::Ok) st = check_rights(a, Right::Rename);
    if (st == Status::Ok) st = check_rights(b, Right::Rename);
    if (st == Status::Ok && replace) st = check_rights(b, Right::Unlink);
    if (st == Status::Ok) st = check_mutable(a);
    if (st == Status::Ok) st = check_mutable(b);
    if (st == Status::Ok && a->realm != b->realm) st = Status::CrossRealm;
    if (st != Status::Ok) return Done::fail(st);

    RelPath ra, rb;
    st = parse_relative(from, &ra);
    if (st == Status::Ok) st = parse_relative(to, &rb);
    if (st != Status::Ok) return Done::fail(st);
    Node pa, pb;
    Leaf la, lb;
    st = locate(a, ra, &pa, &la);
    if (st == Status::Ok) st = locate(b, rb, &pb, &lb);
    if (st != Status::Ok) return Done::fail(st);

    Node src;
    st = from_nebula(a->realm->find(pa, la.name, &src));
    if (st != Status::Ok) return Done::fail(st);
    Node dst;
    nebula::Status f = b->realm->find(pb, lb.name, &dst);
    bool dst_exists = f == nebula::Status::Ok;
    if (!dst_exists && f != nebula::Status::NotFound) return Done::fail(from_nebula(f));
    if (dst_exists && same_node(src, dst)) return Done::success();
    if (dst_exists && !replace) return Done::fail(Status::Exists);

    Info inf;
    st = from_nebula(a->realm->info(src, &inf));
    if (st != Status::Ok) return Done::fail(st);

    if (inf.kind == Kind::Dir && passes_through(b->realm, b->node, rb, rb.count - 1, src))
        return Done::fail(Status::Unsupported);
    if (nebula::node_busy(a->realm, src)) return Done::fail(Status::Busy);
    if (dst_exists && nebula::node_busy(b->realm, dst)) return Done::fail(Status::Busy);
    st = from_nebula(a->realm->rename(pa, la.name, pb, lb.name, replace));
    return st == Status::Ok ? Done::success() : Done::fail(st);
}

Done copy(Domain& d, Lens src, Lens dst_dir, const char* dst_rel) {
    Slot* a;
    Status st = data_slot(d, src, &a, true);
    if (st == Status::Ok) st = check_rights(a, Right::Read);
    if (st == Status::Ok) st = check_available(a);
    Slot* b = nullptr;
    if (st == Status::Ok) st = folder_slot(d, dst_dir, &b);
    if (st == Status::Ok) st = check_mutable(b);
    if (st != Status::Ok) return Done::fail(st);
    RelPath rp;
    st = parse_relative(dst_rel, &rp);
    if (st != Status::Ok) return Done::fail(st);
    Node parent;
    Leaf leaf;
    st = locate(b, rp, &parent, &leaf);
    if (st != Status::Ok) return Done::fail(st);

    Node existing;
    nebula::Status f = b->realm->find(parent, leaf.name, &existing);
    bool exists = f == nebula::Status::Ok;
    if (!exists && f != nebula::Status::NotFound) return Done::fail(from_nebula(f));
    st = check_rights(b, exists ? Right::Write : Right::Create);
    if (st != Status::Ok) return Done::fail(st);
    if (exists && a->realm == b->realm && same_node(existing, a->node)) return Done::fail(Status::Exists);

    nebula::Blob blob;
    st = from_nebula(nebula::read_node(a->realm, a->node, &blob));
    if (st != Status::Ok) return Done::fail(st);
    st = from_nebula(nebula::store_node(b->realm, parent, leaf.name, exists, existing, blob.data, blob.size));
    nebula::release(blob);
    return st == Status::Ok ? Done::success() : Done::fail(st);
}

namespace {

Status subtree_contains(Realm* realm, Node root, Node target, u32 depth, bool* found) {
    if (same_node(root, target)) { *found = true; return Status::Ok; }
    if (depth > nebula::MAX_TREE_DEPTH) return Status::Unsupported;
    nebula::Listing l;
    Status st = from_nebula(nebula::list_node(realm, root, &l, false, false));
    if (st != Status::Ok) return st;
    for (u32 i = 0; i < l.count && st == Status::Ok && !*found; ++i) {
        if (l.items[i].kind != Kind::Dir) continue;
        Node child;
        st = from_nebula(realm->find(root, l.items[i].name, &child));
        if (st == Status::Ok) st = subtree_contains(realm, child, target, depth + 1, found);
    }
    nebula::release(l);
    return st;
}

Status make_dir_at(Realm* realm, Node parent, const char* name, Node* out) {
    nebula::Status f = realm->find(parent, name, out);
    if (f == nebula::Status::NotFound) f = realm->make_dir(parent, name, out);
    if (f != nebula::Status::Ok) return from_nebula(f);
    Info inf;
    Status st = from_nebula(realm->info(*out, &inf));
    if (st != Status::Ok) return st;
    return inf.kind == Kind::Dir ? Status::Ok : Status::NotADirectory;
}

Status copy_below(Realm* from, Node src_dir, Realm* to, Node dst_dir, u32 depth, u32* copied) {
    if (depth > nebula::MAX_TREE_DEPTH) return Status::Unsupported;
    nebula::Listing l;
    Status st = from_nebula(nebula::list_node(from, src_dir, &l, false, false));
    if (st != Status::Ok) return st;
    for (u32 i = 0; i < l.count && st == Status::Ok; ++i) {
        const nebula::Entry& e = l.items[i];
        Node child;
        st = from_nebula(from->find(src_dir, e.name, &child));
        if (st != Status::Ok) break;
        if (e.kind == Kind::Dir) {
            Node made;
            st = make_dir_at(to, dst_dir, e.name, &made);
            if (st == Status::Ok) st = copy_below(from, child, to, made, depth + 1, copied);
        } else {
            nebula::Blob blob;
            st = from_nebula(nebula::read_node(from, child, &blob));
            if (st == Status::Ok) {
                Node existing;
                nebula::Status f = to->find(dst_dir, e.name, &existing);
                bool exists = f == nebula::Status::Ok;
                if (!exists && f != nebula::Status::NotFound) st = from_nebula(f);
                if (st == Status::Ok) st = from_nebula(nebula::store_node(to, dst_dir, e.name, exists, existing, blob.data, blob.size));
                if (st == Status::Ok && copied) ++*copied;
                nebula::release(blob);
            }
        }
    }
    nebula::release(l);
    return st;
}

}

Done put(Domain& d, Lens dir, const char* rel, ConstBytes data) {
    Slot* s;
    Status st = folder_slot(d, dir, &s);
    if (st == Status::Ok) st = check_mutable(s);
    if (st != Status::Ok) return Done::fail(st);
    RelPath rp;
    st = parse_relative(rel, &rp);
    if (st != Status::Ok) return Done::fail(st);
    Node parent;
    Leaf leaf;
    st = locate(s, rp, &parent, &leaf);
    if (st != Status::Ok) return Done::fail(st);
    Node existing;
    nebula::Status f = s->realm->find(parent, leaf.name, &existing);
    bool exists = f == nebula::Status::Ok;
    if (!exists && f != nebula::Status::NotFound) return Done::fail(from_nebula(f));
    st = check_rights(s, exists ? Right::Write : Right::Create);
    if (st != Status::Ok) return Done::fail(st);
    st = from_nebula(nebula::store_node(s->realm, parent, leaf.name, exists, existing, data.ptr, data.len));
    return st == Status::Ok ? Done::success() : Done::fail(st);
}

Done copy_tree(Domain& d, Lens src, Lens dst_dir, const char* dst_rel, u32* copied) {
    if (copied) *copied = 0;
    Slot* a;
    Status st = folder_slot(d, src, &a);
    if (st == Status::Ok) st = check_rights(a, Right::Read | Right::Enumerate);
    if (st == Status::Ok) st = check_available(a);
    Slot* b = nullptr;
    if (st == Status::Ok) st = folder_slot(d, dst_dir, &b);
    if (st == Status::Ok) st = check_rights(b, Right::Create);
    if (st == Status::Ok) st = check_mutable(b);
    if (st != Status::Ok) return Done::fail(st);
    RelPath rp;
    st = parse_relative(dst_rel, &rp);
    if (st != Status::Ok) return Done::fail(st);
    if (rp.count == 0) return Done::fail(Status::InvalidPath);
    if (b->depth + rp.count > MAX_DEPTH) return Done::fail(Status::InvalidPath);

    if (a->realm == b->realm) {
        if (passes_through(b->realm, b->node, rp, rp.count, a->node)) return Done::fail(Status::Unsupported);
        bool inside = false;
        st = subtree_contains(a->realm, a->node, b->node, 0, &inside);
        if (st != Status::Ok) return Done::fail(st);
        if (inside) return Done::fail(Status::Unsupported);
    }

    Node cur = b->node;
    for (u32 i = 0; i < rp.count; ++i) {
        Info inf;
        st = from_nebula(b->realm->info(cur, &inf));
        if (st != Status::Ok) return Done::fail(st);
        if (inf.kind != Kind::Dir) return Done::fail(Status::NotADirectory);
        char name[nebula::NAME_BYTES + 1];
        copy_name(name, rp.comps[i].p, rp.comps[i].n);
        Node next;
        st = make_dir_at(b->realm, cur, name, &next);
        if (st != Status::Ok) return Done::fail(st);
        cur = next;
    }
    st = copy_below(a->realm, a->node, b->realm, cur, 0, copied);
    return st == Status::Ok ? Done::success() : Done::fail(st);
}

}
