#include <cosmos/nebula_realms.hpp>
#include <cosmos/kheap.hpp>
#include <cosmos/orbital.hpp>
#include <cosmos/clock.hpp>

namespace nebula {

namespace {

struct Item {
    bool used;
    Kind kind;
    u32 gen;
    u32 parent;
    char name[NAME_BYTES + 1];
    u8* data;
    u64 size;
    u64 capacity;
    u64 mtime;
};

class RamRealm final : public Realm {
public:
    const char* name() const override { return "ram"; }
    const char* summary() const override { return "volatile memory, lost at power-off"; }
    bool writable() const override { return true; }
    bool available() const override { return true; }

    Node root() override {
        Guard g(lock_);
        if (!ensure()) return Node{0, 0};
        return Node{0, items_[0].gen};
    }

    Status find(Node dir, const char* name, Node* out) override {
        Guard g(lock_);
        Item* d = get_dir(dir);
        if (!d) return dir_error(dir);
        u32 i = child_index(static_cast<u32>(dir.id), name);
        if (i == NONE) return Status::NotFound;
        *out = Node{i, items_[i].gen};
        return Status::Ok;
    }

    Status info(Node node, Info* out) override {
        Guard g(lock_);
        Item* it = get(node);
        if (!it) return Status::NotFound;
        out->kind = it->kind;
        out->size = it->kind == Kind::File ? it->size : 0;
        out->mtime_ms = it->mtime;
        out->links = 1;
        out->read_only = false;
        out->dynamic = false;
        return Status::Ok;
    }

    Status list(Node dir, ChildFn fn, void* ctx) override {
        Guard g(lock_);
        Item* d = get_dir(dir);
        if (!d) return dir_error(dir);
        for (u32 i = 1; i < RAM_MAX_ITEMS; ++i) {
            Item& it = items_[i];
            if (it.used && it.parent == dir.id) fn(it.name, it.kind, Node{i, it.gen}, ctx);
        }
        return Status::Ok;
    }

    Status read_all(Node file, void* buf, u64 capacity, u64* size) override {
        Guard g(lock_);
        Item* it = get(file);
        if (!it) return Status::NotFound;
        if (it->kind != Kind::File) return Status::IsADirectory;
        u64 n = it->size < capacity ? it->size : capacity;
        if (n) __builtin_memcpy(buf, it->data, static_cast<usize>(n));
        *size = it->size;
        return Status::Ok;
    }

    Status write_all(Node file, const void* data, u64 size) override {
        Guard g(lock_);
        Item* it = get(file);
        if (!it) return Status::NotFound;
        if (it->kind != Kind::File) return Status::IsADirectory;
        if (size > RAM_LIMIT_BYTES) return Status::NoSpace;
        if (bytes_ - it->size + size > RAM_LIMIT_BYTES) return Status::NoSpace;
        if (size > it->capacity) {
            u8* fresh = static_cast<u8*>(kheap::alloc(static_cast<usize>(size)));
            if (!fresh) return Status::NoMemory;
            if (it->data) kheap::free(it->data);
            it->data = fresh;
            it->capacity = size;
        }
        if (size) __builtin_memcpy(it->data, data, static_cast<usize>(size));
        bytes_ = bytes_ - it->size + size;
        it->size = size;
        it->mtime = clock::now_ms();
        return Status::Ok;
    }

    Status create_file(Node dir, const char* name, Node* out) override { return create(dir, name, Kind::File, out); }
    Status make_dir(Node dir, const char* name, Node* out) override { return create(dir, name, Kind::Dir, out); }

    Status remove(Node dir, const char* name) override {
        Guard g(lock_);
        Item* d = get_dir(dir);
        if (!d) return dir_error(dir);
        u32 i = child_index(static_cast<u32>(dir.id), name);
        if (i == NONE) return Status::NotFound;
        Item& it = items_[i];
        if (it.kind == Kind::Dir && has_children(i)) return Status::NotEmpty;
        discard(i);
        d->mtime = clock::now_ms();
        return Status::Ok;
    }

    Status rename(Node from_dir, const char* from, Node to_dir, const char* to, bool replace) override {
        Guard g(lock_);
        Item* fd = get_dir(from_dir);
        if (!fd) return dir_error(from_dir);
        Item* td = get_dir(to_dir);
        if (!td) return dir_error(to_dir);
        u32 src = child_index(static_cast<u32>(from_dir.id), from);
        if (src == NONE) return Status::NotFound;
        u32 dst = child_index(static_cast<u32>(to_dir.id), to);
        if (dst == src) return Status::Ok;
        if (items_[src].kind == Kind::Dir && is_inside(static_cast<u32>(to_dir.id), src)) return Status::Unsupported;
        if (dst != NONE) {
            if (!replace) return Status::Exists;
            if (items_[src].kind == Kind::Dir && items_[dst].kind != Kind::Dir) return Status::NotADirectory;
            if (items_[src].kind != Kind::Dir && items_[dst].kind == Kind::Dir) return Status::IsADirectory;
            if (items_[dst].kind == Kind::Dir && has_children(dst)) return Status::NotEmpty;
            discard(dst);
        }
        Item& s = items_[src];
        s.parent = static_cast<u32>(to_dir.id);
        copy(s.name, to);
        u64 now = clock::now_ms();
        s.mtime = now;
        fd->mtime = now;
        td->mtime = now;
        return Status::Ok;
    }

    u64 space_free() override {
        Guard g(lock_);
        return RAM_LIMIT_BYTES - bytes_;
    }
    u64 space_total() override { return RAM_LIMIT_BYTES; }

private:
    static constexpr u32 NONE = 0xFFFFFFFFu;

    struct Guard {
        explicit Guard(orbital::Mutex& m) : m_(m) { m_.lock(); }
        ~Guard() { m_.unlock(); }
        orbital::Mutex& m_;
    };

    static void copy(char* dst, const char* src) {
        u32 i = 0;
        for (; src[i] && i < NAME_BYTES; ++i) dst[i] = src[i];
        dst[i] = 0;
    }

    static bool same(const char* a, const char* b) {
        while (*a && *a == *b) { ++a; ++b; }
        return *a == *b;
    }

    bool ensure() {
        if (items_) return true;
        items_ = static_cast<Item*>(kheap::alloc_zeroed(sizeof(Item) * RAM_MAX_ITEMS));
        if (!items_) return false;
        items_[0].used = true;
        items_[0].kind = Kind::Dir;
        items_[0].gen = next_gen_++;
        items_[0].mtime = clock::now_ms();
        return true;
    }

    Item* get(Node n) {
        if (!ensure() || n.id >= RAM_MAX_ITEMS) return nullptr;
        Item& it = items_[n.id];
        if (!it.used || it.gen != n.scope) return nullptr;
        return &it;
    }

    Item* get_dir(Node n) {
        Item* it = get(n);
        return it && it->kind == Kind::Dir ? it : nullptr;
    }

    Status dir_error(Node n) {
        Item* it = get(n);
        if (!it) return Status::NotFound;
        return Status::NotADirectory;
    }

    u32 child_index(u32 parent, const char* name) {
        for (u32 i = 1; i < RAM_MAX_ITEMS; ++i)
            if (items_[i].used && items_[i].parent == parent && same(items_[i].name, name)) return i;
        return NONE;
    }

    bool has_children(u32 index) {
        for (u32 i = 1; i < RAM_MAX_ITEMS; ++i)
            if (items_[i].used && items_[i].parent == index) return true;
        return false;
    }

    bool is_inside(u32 dir, u32 ancestor) {
        u32 cur = dir;
        for (u32 hops = 0; hops <= RAM_MAX_ITEMS; ++hops) {
            if (cur == ancestor) return true;
            if (cur == 0) return false;
            cur = items_[cur].parent;
        }
        return true;
    }

    void discard(u32 index) {
        Item& it = items_[index];
        if (it.data) kheap::free(it.data);
        bytes_ -= it.size;
        it = Item{};
    }

    Status create(Node dir, const char* name, Kind kind, Node* out) {
        Guard g(lock_);
        Item* d = get_dir(dir);
        if (!d) return dir_error(dir);
        if (!path::name_ok(name, path::text_length(name))) return Status::InvalidName;
        if (child_index(static_cast<u32>(dir.id), name) != NONE) return Status::Exists;
        u32 slot = NONE;
        for (u32 i = 1; i < RAM_MAX_ITEMS; ++i)
            if (!items_[i].used) { slot = i; break; }
        if (slot == NONE) return Status::NoSpace;
        Item& it = items_[slot];
        it = Item{};
        it.used = true;
        it.kind = kind;
        it.gen = next_gen_++;
        if (next_gen_ == 0) next_gen_ = 1;
        it.parent = static_cast<u32>(dir.id);
        copy(it.name, name);
        it.mtime = clock::now_ms();
        d->mtime = it.mtime;
        *out = Node{slot, it.gen};
        return Status::Ok;
    }

    Item* items_ = nullptr;
    u32 next_gen_ = 1;
    u64 bytes_ = 0;
    orbital::Mutex lock_;
};

constinit RamRealm g_ram;

}

Realm* ram_realm() { return &g_ram; }

}
