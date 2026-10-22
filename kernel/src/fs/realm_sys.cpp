#include <cosmos/nebula_realms.hpp>
#include <cosmos/kheap.hpp>
#include <cosmos/orbital.hpp>
#include <cosmos/clock.hpp>
#include <cosmos/klog.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/power.hpp>

namespace nebula {

namespace {

constexpr u32 RENDER_CAP = 24576;

const char* g_storage_text = "unknown";
u32 g_suns = 1;

struct Text {
    char* out;
    u32 cap;
    u32 n;

    void put(char c) { if (n < cap) out[n++] = c; }
    void str(const char* s) { while (*s) put(*s++); }
    void num(u64 v) {
        char t[24];
        u32 i = 0;
        if (!v) t[i++] = '0';
        while (v) { t[i++] = static_cast<char>('0' + v % 10); v /= 10; }
        while (i) put(t[--i]);
    }
    void pad(u64 v, u32 width) {
        u32 digits = 1;
        for (u64 x = v; x >= 10; x /= 10) ++digits;
        for (u32 i = digits; i < width; ++i) put(' ');
        num(v);
    }
    void line(const char* key, u64 value, const char* unit) {
        str(key);
        str(": ");
        num(value);
        if (unit) { put(' '); str(unit); }
        put('\n');
    }
};

const char* state_name(orbital::State s) {
    switch (s) {
        case orbital::State::Ready: return "ready";
        case orbital::State::Running: return "running";
        case orbital::State::Sleeping: return "sleeping";
        case orbital::State::Blocked: return "blocked";
    }
    return "?";
}

void render_version(Text& t) {
    t.str("Aphelion x86_64 C++20 non-POSIX kernel\n");
    t.line("suns", g_suns, nullptr);
    power::Capabilities caps = power::capabilities();
    t.str("hypervisor: ");
    t.str(caps.hypervisor ? "yes\n" : "no\n");
}

void render_uptime(Text& t) {
    u64 ms = clock::uptime_ms();
    t.line("uptime_ms", ms, nullptr);
    u64 s = ms / 1000;
    t.str("up ");
    t.num(s / 3600);
    t.str("h ");
    t.num((s / 60) % 60);
    t.str("m ");
    t.num(s % 60);
    t.str("s\n");
}

void render_memory(Text& t) {
    kheap::Stats h = kheap::stats();
    t.line("universe_free", universe::free_bytes() >> 10, "KiB");
    t.line("universe_total", universe::total_bytes() >> 10, "KiB");
    t.line("galaxies", universe::galaxy_count(), nullptr);
    t.line("heap_objects", h.live_objects, nullptr);
    t.line("heap_bytes", h.live_bytes, nullptr);
    t.line("heap_failed_allocs", h.failed_allocs, nullptr);
    t.line("heap_slab_pages", h.slab_pages, nullptr);
    t.line("heap_large_blocks", h.large_blocks, nullptr);
}

void render_cpus(Text& t) {
    orbital::CoreInfo info[64];
    u32 n = orbital::cores(info, 64);
    orbital::Stats st = orbital::stats();
    for (u32 i = 0; i < n; ++i) {
        t.str("sun ");
        t.num(i);
        t.str(" apic ");
        t.num(info[i].apic_id);
        t.str(info[i].idle ? " idle" : " running");
        t.str(" queued ");
        t.num(info[i].queued);
        t.put(' ');
        t.str(info[i].running);
        t.put('\n');
    }
    t.line("preemptions", st.preemptions, nullptr);
    t.line("steals", st.steals, nullptr);
    t.line("ipis", st.ipis_sent, nullptr);
    t.line("boosts", st.boosts, nullptr);
}

void render_tasks(Text& t) {
    orbital::SatelliteInfo info[64];
    u32 n = orbital::snapshot(info, 64);
    for (u32 i = 0; i < n; ++i) {
        t.pad(info[i].id, 3);
        t.put(' ');
        t.str(state_name(info[i].state));
        t.str(" ring ");
        t.num(static_cast<u64>(info[i].ring));
        t.str(" core ");
        t.num(info[i].core);
        t.str(" switches ");
        t.num(info[i].switches);
        t.put(' ');
        t.str(info[i].name);
        t.put('\n');
    }
}

void render_realms(Text& t) {
    for (u32 i = 0; i < realm_count(); ++i) {
        Realm* r = realm_at(i);
        t.str(r->name());
        t.str(r->available() ? " available " : " unavailable ");
        t.str(r->writable() ? "writable " : "read-only ");
        t.str(r->summary());
        t.put('\n');
    }
}

void render_storage(Text& t) {
    t.str(g_storage_text);
    t.put('\n');
    Realm* d = disk_realm();
    if (d->available()) {
        t.line("disk_free", d->space_free() >> 20, "MiB");
        t.line("disk_total", d->space_total() >> 20, "MiB");
        t.line("snapshots", stellar::snapshot_count(), nullptr);
    }
}

void render_log(Text& t) {
    u64 tail = klog::tail();
    u64 seq = tail > 8192 ? tail - 8192 : 0;
    char chunk[256];
    for (;;) {
        u32 n = klog::read(seq, chunk, sizeof(chunk));
        if (!n) break;
        for (u32 i = 0; i < n; ++i) {
            char c = chunk[i];
            if (c == '\n' || (c >= 0x20 && c < 0x7F) || static_cast<u8>(c) >= 0x80) t.put(c);
        }
    }
}

struct SysFile {
    const char* name;
    void (*render)(Text&);
};

constexpr SysFile FILES[] = {
    {"version", &render_version},
    {"uptime", &render_uptime},
    {"memory", &render_memory},
    {"cpus", &render_cpus},
    {"tasks", &render_tasks},
    {"realms", &render_realms},
    {"storage", &render_storage},
    {"log", &render_log},
};
constexpr u32 FILE_COUNT = sizeof(FILES) / sizeof(FILES[0]);

bool same(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

class SysRealm final : public Realm {
public:
    const char* name() const override { return "sys"; }
    const char* summary() const override { return "live kernel state as read-only text files"; }
    bool writable() const override { return false; }
    bool available() const override { return true; }
    Node root() override { return Node{0, 1}; }

    Status find(Node dir, const char* name, Node* out) override {
        if (dir.id != 0) return Status::NotADirectory;
        for (u32 i = 0; i < FILE_COUNT; ++i)
            if (same(FILES[i].name, name)) { *out = Node{i + 1, 1}; return Status::Ok; }
        return Status::NotFound;
    }

    Status info(Node node, Info* out) override {
        if (node.scope != 1 || node.id > FILE_COUNT) return Status::NotFound;
        out->read_only = true;
        out->links = 1;
        out->mtime_ms = clock::now_ms();
        if (node.id == 0) {
            out->kind = Kind::Dir;
            out->size = 0;
            out->dynamic = false;
            return Status::Ok;
        }
        u64 n = 0;
        Status st = render(static_cast<u32>(node.id - 1), nullptr, 0, &n);
        if (st != Status::Ok) return st;
        out->kind = Kind::File;
        out->size = n;
        out->dynamic = true;
        return Status::Ok;
    }

    Status list(Node dir, ChildFn fn, void* ctx) override {
        if (dir.id != 0) return Status::NotADirectory;
        for (u32 i = 0; i < FILE_COUNT; ++i) fn(FILES[i].name, Kind::File, Node{i + 1, 1}, ctx);
        return Status::Ok;
    }

    Status read_all(Node file, void* buf, u64 capacity, u64* size) override {
        if (file.scope != 1 || file.id == 0 || file.id > FILE_COUNT) return Status::NotFound;
        return render(static_cast<u32>(file.id - 1), buf, capacity, size);
    }

    Status write_all(Node, const void*, u64) override { return Status::ReadOnly; }
    Status create_file(Node, const char*, Node*) override { return Status::ReadOnly; }
    Status make_dir(Node, const char*, Node*) override { return Status::ReadOnly; }
    Status remove(Node, const char*) override { return Status::ReadOnly; }
    Status rename(Node, const char*, Node, const char*, bool) override { return Status::ReadOnly; }

private:
    Status render(u32 index, void* buf, u64 capacity, u64* size) {
        char* scratch = static_cast<char*>(kheap::alloc(RENDER_CAP));
        if (!scratch) return Status::NoMemory;
        Text t{scratch, RENDER_CAP, 0};
        FILES[index].render(t);
        u64 n = t.n < capacity ? t.n : capacity;
        if (buf && n) __builtin_memcpy(buf, scratch, static_cast<usize>(n));
        *size = buf ? n : t.n;
        kheap::free(scratch);
        return Status::Ok;
    }
};

constinit SysRealm g_sys;

}

Realm* sys_realm() { return &g_sys; }

void sys_set_facts(const char* storage, u32 suns) {
    g_storage_text = storage ? storage : "unknown";
    g_suns = suns ? suns : 1;
}

bool register_builtin_realms() {
    bool ok = true;
    ok = register_realm(&g_sys) && ok;
    ok = register_realm(ram_realm()) && ok;
    ok = register_realm(disk_realm()) && ok;
    ok = register_realm(snap_realm()) && ok;
    return ok;
}

}
