#include <cosmos/types.hpp>
#include <cosmos/blockdev.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/stellar.hpp>
#include <cosmos/orbital.hpp>
#include <cosmos/kheap.hpp>
#include <cosmos/clock.hpp>
#include <cosmos/nebula.hpp>
#include <cosmos/nebula_realms.hpp>
#include <cosmos/lens.hpp>
#include <cosmos/lens_text.hpp>

extern "C" {
int printf(const char*, ...);
int vprintf(const char*, __builtin_va_list);
int puts(const char*);
void* malloc(unsigned long);
void* aligned_alloc(unsigned long, unsigned long);
void* calloc(unsigned long, unsigned long);
void free(void*);
int sched_yield(void);
int usleep(unsigned);
typedef unsigned long pthread_t;
int pthread_create(pthread_t*, const void*, void* (*)(void*), void*);
int pthread_join(pthread_t, void**);
}

static u8* g_disk;
static u64 g_sectors;
static u64 g_wcount = 0;
static u64 g_last_sb_w = 0;
static u64 g_fail_after = ~0ull;

namespace blockdev {
bool read_sector(u64 s, void* b) { if (s >= g_sectors) return false; mem::copy(b, g_disk + s * 512, 512); return true; }
bool write_sector(u64 s, const void* b) {
    if (s >= g_sectors || g_wcount >= g_fail_after) return false;
    ++g_wcount;
    if (s < 2) g_last_sb_w = g_wcount;
    mem::copy(g_disk + s * 512, b, 512);
    return true;
}
bool read_sectors(u64 s, u64 n, void* b) { if (s + n > g_sectors) return false; mem::copy(b, g_disk + s * 512, n * 512); return true; }
bool write_sectors(u64 s, u64 n, const void* b) {
    if (s + n > g_sectors) return false;
    u64 ok_n = n;
    if (g_wcount + n > g_fail_after) ok_n = g_fail_after > g_wcount ? g_fail_after - g_wcount : 0;
    for (u64 i = 0; i < ok_n; ++i) if (s + i < 2) g_last_sb_w = g_wcount + i + 1;
    mem::copy(g_disk + s * 512, b, ok_n * 512);
    g_wcount += ok_n;
    return ok_n == n;
}
u64 capacity_sectors() { return g_sectors; }
bool flush() { return true; }
}

namespace orbital {
void yield() { sched_yield(); }
void yield_contended() { sched_yield(); }
u64 self_token() { static thread_local char tag; return reinterpret_cast<u64>(&tag); }
}

namespace universe {
u64 alloc(int order) { return reinterpret_cast<u64>(aligned_alloc(4096, PAGE_SIZE << order)); }
void free(u64, int) {}
}

namespace kheap {
void* alloc(usize n) { return malloc(n ? n : 1); }
void* alloc_zeroed(usize n) { return calloc(n ? n : 1, 1); }
void free(void* p) { ::free(p); }
}

static u64 g_clock = 1000;
namespace clock { u64 now_ms() { return g_clock += 7; } }

namespace serial {
void printf(const char*, ...) {}
void writeln(const char*) {}
}

#include <cosmos/klog.hpp>
#include <cosmos/power.hpp>
struct host_timespec { long sec; long nsec; };
extern "C" int clock_gettime(int, host_timespec*);
static u64 g_uptime_fixed = 0;
static u64 host_ms() { host_timespec ts; clock_gettime(1, &ts); return static_cast<u64>(ts.sec) * 1000 + static_cast<u64>(ts.nsec) / 1000000; }
namespace clock { u64 uptime_ms() { return g_uptime_fixed ? g_uptime_fixed : host_ms(); } }
namespace universe {
u64 free_bytes() { return 100ull << 20; }
u64 total_bytes() { return 256ull << 20; }
u32 galaxy_count() { return 2; }
}
namespace kheap { Stats stats() { Stats s{}; s.live_objects = 12; s.live_bytes = 3456; s.slab_pages = 7; return s; } }
namespace power { Capabilities capabilities() { Capabilities c{}; c.hypervisor = true; return c; } }
namespace klog {
static const char g_text[] = "boot: hello\nstellar: mounted: ok\n";
u64 tail() { return sizeof(g_text) - 1; }
u32 read(u64& seq, char* out, u32 max) {
    if (seq >= sizeof(g_text) - 1) return 0;
    u32 n = static_cast<u32>(sizeof(g_text) - 1 - seq);
    if (n > max) n = max;
    mem::copy(out, g_text + seq, n);
    seq += n;
    return n;
}
}
namespace orbital {

bool WaitQueue::wait_until_for(bool (*ready)(void*), void* arg, u64 timeout_ms) {
    u64 start = host_ms();
    for (;;) {
        if (ready(arg)) return true;
        if (timeout_ms != FOREVER && host_ms() - start >= timeout_ms) return false;
        usleep(300);
    }
}
u32 WaitQueue::wake_all() { return 0; }
u32 cores(CoreInfo* out, u32 max) {
    if (max < 2) return 0;
    out[0] = CoreInfo{}; out[0].apic_id = 0; out[0].idle = false; out[0].queued = 2; cstr::copy(out[0].running, "helm");
    out[1] = CoreInfo{}; out[1].apic_id = 1; out[1].idle = true; out[1].queued = 0; cstr::copy(out[1].running, "idle-1");
    return 2;
}
u32 snapshot(SatelliteInfo* out, u32 max) {
    if (max < 2) return 0;
    out[0] = SatelliteInfo{}; out[0].id = 1; out[0].state = State::Running; out[0].ring = 0; out[0].core = 0; out[0].switches = 40; cstr::copy(out[0].name, "helm");
    out[1] = SatelliteInfo{}; out[1].id = 12; out[1].state = State::Sleeping; out[1].ring = 3; out[1].core = 1; out[1].switches = 5; cstr::copy(out[1].name, "net rx");
    return 2;
}
Stats stats() { Stats s{}; s.preemptions = 9; s.steals = 3; s.ipis_sent = 4; s.boosts = 1; return s; }
}

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(c, msg) do { ++g_checks; if (!(c)) { ++g_fail; printf("FAIL: %s  (line %d)\n", msg, __LINE__); } } while (0)
#define CHECK_L(res, want, msg) do { auto r_ = (res); ++g_checks; if (r_.status() != (want)) { ++g_fail; printf("FAIL: %s  got '%s' want '%s' (line %d)\n", msg, lens::status_name(r_.status()), lens::status_name(want), __LINE__); } } while (0)

using Status_n = nebula::Status;
using lens::Domain;
using lens::Epoch;
using lens::Ready;
using lens::Lens;
using lens::Rights;
using lens::Right;
using lens::Status;
using lens::NodeKind;

static bool same(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

static u64 g_probe_ticks;

class ProbeRealm final : public nebula::Realm {
public:
    const char* name() const override { return "probe"; }
    const char* summary() const override { return "test double for computed key: value files"; }
    bool writable() const override { return false; }
    bool available() const override { return true; }
    nebula::Node root() override { return nebula::Node{0, 1}; }
    Status_n find(nebula::Node dir, const char* n, nebula::Node* out) override {
        if (dir.id != 0) return Status_n::NotADirectory;
        if (same(n, "version")) { *out = nebula::Node{1, 1}; return Status_n::Ok; }
        if (same(n, "ticks")) { *out = nebula::Node{2, 1}; return Status_n::Ok; }
        return Status_n::NotFound;
    }
    Status_n info(nebula::Node n, nebula::Info* out) override {
        out->read_only = true; out->links = 1; out->mtime_ms = 0;
        if (n.id == 0) { out->kind = nebula::Kind::Dir; out->size = 0; out->dynamic = false; return Status_n::Ok; }
        if (n.id > 2) return Status_n::NotFound;
        u64 got = 0;
        char tmp[128];
        read_all(n, tmp, sizeof(tmp), &got);
        out->kind = nebula::Kind::File; out->size = got; out->dynamic = true;
        return Status_n::Ok;
    }
    Status_n list(nebula::Node dir, nebula::ChildFn fn, void* ctx) override {
        if (dir.id != 0) return Status_n::NotADirectory;
        fn("version", nebula::Kind::File, nebula::Node{1, 1}, ctx);
        fn("ticks", nebula::Kind::File, nebula::Node{2, 1}, ctx);
        return Status_n::Ok;
    }
    Status_n read_all(nebula::Node f, void* buf, u64 cap, u64* size) override {
        char t[128];
        if (f.id == 1) __builtin_snprintf(t, sizeof(t), "name: aphelion\nkind: test\n");
        else if (f.id == 2) __builtin_snprintf(t, sizeof(t), "ticks: %llu\n", g_probe_ticks++);
        else return Status_n::NotFound;
        u64 n = cstr::length(t);
        if (n > cap) n = cap;
        mem::copy(buf, t, n);
        *size = n;
        return Status_n::Ok;
    }
    Status_n write_all(nebula::Node, const void*, u64) override { return Status_n::ReadOnly; }
    Status_n create_file(nebula::Node, const char*, nebula::Node*) override { return Status_n::ReadOnly; }
    Status_n make_dir(nebula::Node, const char*, nebula::Node*) override { return Status_n::ReadOnly; }
    Status_n remove(nebula::Node, const char*) override { return Status_n::ReadOnly; }
    Status_n rename(nebula::Node, const char*, nebula::Node, const char*, bool) override { return Status_n::ReadOnly; }
};
static ProbeRealm g_probe;

static nebula::Session g_sess;

static bool put(const char* path, const char* text) {
    return nebula::write_file(g_sess, path, text, cstr::length(text)) == nebula::Status::Ok;
}

static bool mk(const char* path) { return nebula::make_path(g_sess, path) == nebula::Status::Ok; }

static void fixture(const char* realm) {
    char b[128];
    auto p = [&](const char* rel) { __builtin_snprintf(b, sizeof(b), "%s:/%s", realm, rel); return b; };
    nebula::remove_tree(g_sess, p("proj"), nullptr);
    mk(p("proj/src/deep"));
    put(p("proj/readme.txt"), "hello lens");
    put(p("proj/src/main.c"), "int main;");
    put(p("proj/src/deep/leaf.txt"), "leaf");
    put(p("proj/a:b"), "colon name");
}

static Domain* new_domain() { return static_cast<Domain*>(calloc(1, sizeof(Domain))); }

static Lens must_mint(Domain& d, const char* text, Rights r, bool ro = false) {
    auto m = lens::mint(d, text, r, ro);
    if (!m.ok()) { printf("   mint('%s') failed: %s\n", text, lens::status_name(m.status())); ++g_fail; }
    return m.value();
}

static Lens must_open(Domain& d, Lens dir, const char* rel, Rights r) {
    auto m = lens::open_at(d, dir, rel, r);
    if (!m.ok()) { printf("   open_at('%s') failed: %s\n", rel, lens::status_name(m.status())); ++g_fail; }
    return m.value();
}

static const Rights ALL = Rights::all();

static void test_mint() {
    Domain* d = new_domain();
    lens::test_unseal();

    Lens root = must_mint(*d, "ram:/", ALL);
    CHECK(!root.is_null(), "mint ram root");
    CHECK(lens::kind_of(*d, root).value() == NodeKind::Folder, "root is a folder");
    char lab[64];
    lens::label_of(*d, root, lab, sizeof(lab));
    CHECK(same(lab, "ram"), "root label is the realm");
    CHECK(lens::depth_of(*d, root).value() == 0, "root depth 0");

    Lens sub = must_mint(*d, "ram:/proj/src", Right::Read | Right::Enumerate);
    CHECK(lens::kind_of(*d, sub).value() == NodeKind::Folder, "mint at a subdirectory");
    lens::label_of(*d, sub, lab, sizeof(lab));
    CHECK(same(lab, "src"), "subdirectory label");
    CHECK(lens::rights_of(*d, sub).value() == (Right::Read | Right::Enumerate), "mint keeps requested rights");

    Lens file = must_mint(*d, "ram:/proj/readme.txt", Right::Read);
    CHECK(lens::kind_of(*d, file).value() == NodeKind::Blob, "mint on a file is a blob");

    Lens probe = must_mint(*d, "probe:/version", Right::Read);
    CHECK(lens::kind_of(*d, probe).value() == NodeKind::Probe, "computed files are probes");

    lens::Result<Lens> bad;
    CHECK_L(lens::mint(*d, "proj/src", ALL), Status::InvalidPath, "mint without a realm prefix");
    CHECK_L(lens::mint(*d, "/proj", ALL), Status::InvalidPath, "mint with absolute text only");
    CHECK_L(lens::mint(*d, "", ALL), Status::InvalidPath, "mint empty");
    CHECK_L(lens::mint(*d, nullptr, ALL), Status::InvalidPath, "mint null");
    CHECK_L(lens::mint(*d, "ram:/proj/..", ALL), Status::Escape, "mint refuses dotdot");
    CHECK_L(lens::mint(*d, "ram:/proj/src/../..", ALL), Status::Escape, "mint refuses dotdot chains");
    CHECK_L(lens::mint(*d, "nope:/x", ALL), Status::Unavailable, "mint unknown realm");
    CHECK_L(lens::mint(*d, "ram:/missing", ALL), Status::NotFound, "mint missing path");
    CHECK_L(lens::mint(*d, "ram:/proj/readme.txt/x", ALL), Status::NotADirectory, "mint through a file");
    CHECK_L(lens::mint(*d, "ram:/pr\x01oj", ALL), Status::InvalidName, "mint with a control char");
    CHECK_L(lens::mint(*d, "Ram:/x", ALL), Status::InvalidPath, "mint with a bad realm name");

    nebula::disk_set_mounted(false);
    Lens droot = must_mint(*d, "disk:/", ALL);
    CHECK(!droot.is_null(), "unmounted disk root mints");
    CHECK_L(lens::mint(*d, "disk:/proj", ALL), Status::Unavailable, "unmounted disk subpath refused");
    CHECK_L(lens::stat(*d, droot), Status::Unavailable, "stat on an unmounted realm");
    nebula::disk_set_mounted(true);
    CHECK(lens::stat(*d, droot).ok(), "same lens works once the disk mounts");

    CHECK(!lens::sealed(), "not sealed yet");
    lens::seal();
    CHECK(lens::sealed(), "sealed");
    CHECK_L(lens::mint(*d, "ram:/", ALL), Status::Denied, "mint after seal");
    CHECK_L(lens::mint(*d, "probe:/", Rights::none()), Status::Denied, "mint after seal, any rights");
    lens::test_unseal();
    CHECK(lens::mint(*d, "ram:/", ALL).ok(), "unseal restores minting for tests");

    free(d);
}

static void test_open_at() {
    Domain* d = new_domain();
    lens::test_unseal();
    Lens root = must_mint(*d, "ram:/proj", ALL);

    Lens src = must_open(*d, root, "src", ALL);
    CHECK(lens::kind_of(*d, src).value() == NodeKind::Folder && lens::depth_of(*d, src).value() == 1, "one level down");
    Lens leaf = must_open(*d, root, "src/deep/leaf.txt", Right::Read);
    CHECK(lens::kind_of(*d, leaf).value() == NodeKind::Blob && lens::depth_of(*d, leaf).value() == 3, "three levels down");
    Lens again = must_open(*d, src, "deep/leaf.txt", Right::Read);
    CHECK(lens::stat(*d, again).value().size == 4, "same node reached from a lower lens");
    CHECK(must_open(*d, root, "./src//deep/./", ALL).raw != 0, "dots and double slashes are skipped");
    Lens self = must_open(*d, src, ".", Right::Read);
    CHECK(lens::depth_of(*d, self).value() == 1 && lens::kind_of(*d, self).value() == NodeKind::Folder, "dot is a copy of the same lens");
    char lab[64];
    lens::label_of(*d, self, lab, sizeof(lab));
    CHECK(same(lab, "src"), "dot keeps the label");
    CHECK(must_open(*d, root, "./a:b", Right::Read).raw != 0, "a colon after a slash is just a name");

    CHECK_L(lens::open_at(*d, src, "..", ALL), Status::Escape, "dotdot");
    CHECK_L(lens::open_at(*d, src, "../readme.txt", Right::Read), Status::Escape, "dotdot then name");
    CHECK_L(lens::open_at(*d, src, "deep/../..", Right::Read), Status::Escape, "dotdot after a descent");
    CHECK_L(lens::open_at(*d, src, "deep/..", Right::Read), Status::Escape, "dotdot that would stay inside is still refused");
    CHECK_L(lens::open_at(*d, src, "../../../..", Right::Read), Status::Escape, "dotdot chain");
    CHECK_L(lens::open_at(*d, src, "/", Right::Read), Status::Escape, "absolute root");
    CHECK_L(lens::open_at(*d, src, "/proj/readme.txt", Right::Read), Status::Escape, "absolute path");
    CHECK_L(lens::open_at(*d, src, "ram:/proj", Right::Read), Status::Escape, "realm prefix, same realm");
    CHECK_L(lens::open_at(*d, src, "disk:/proj", Right::Read), Status::Escape, "realm prefix, other realm");
    CHECK_L(lens::open_at(*d, src, "disk:", Right::Read), Status::Escape, "bare realm prefix");
    CHECK_L(lens::open_at(*d, src, "snap:/1", Right::Read), Status::Escape, "snap prefix");
    CHECK_L(lens::open_at(*d, src, "a:b", Right::Read), Status::Escape, "colon before the first slash is always a prefix attempt");
    CHECK_L(lens::open_at(*d, src, "x:", Right::Read), Status::Escape, "trailing colon");
    CHECK_L(lens::open_at(*d, src, ":x", Right::Read), Status::Escape, "leading colon");
    CHECK_L(lens::open_at(*d, src, "deep/..\\x", Right::Read), Status::NotFound, "backslash is an ordinary name byte");
    CHECK_L(lens::open_at(*d, src, "...", Right::Read), Status::NotFound, "three dots is an ordinary name");
    CHECK_L(lens::open_at(*d, src, "..x", Right::Read), Status::NotFound, "dotdot prefix inside a name is fine");
    CHECK(lens::live_count(*d) == 7, "escape attempts allocated nothing");

    CHECK_L(lens::open_at(*d, root, "", Right::Read), Status::InvalidPath, "empty path");
    CHECK_L(lens::open_at(*d, root, nullptr, Right::Read), Status::InvalidPath, "null path");
    CHECK_L(lens::open_at(*d, root, "nothing", Right::Read), Status::NotFound, "missing child");
    CHECK_L(lens::open_at(*d, root, "src/missing/x", Right::Read), Status::NotFound, "missing middle");
    CHECK_L(lens::open_at(*d, root, "readme.txt/x", Right::Read), Status::NotADirectory, "through a file");
    CHECK_L(lens::open_at(*d, leaf, "x", Right::Read), Status::NotADirectory, "open_at from a file lens");
    CHECK_L(lens::open_at(*d, root, "a\x01b", Right::Read), Status::InvalidName, "control character");
    CHECK_L(lens::open_at(*d, root, "\xC0\xAF", Right::Read), Status::InvalidName, "overlong utf-8");
    char fifty2[64];
    for (int i = 0; i < 52; ++i) fifty2[i] = 'a';
    fifty2[52] = 0;
    CHECK_L(lens::open_at(*d, root, fifty2, Right::Read), Status::InvalidName, "52 byte name");
    char longtext[600];
    for (int i = 0; i < 599; ++i) longtext[i] = (i % 4 == 3) ? '/' : 'x';
    longtext[599] = 0;
    CHECK_L(lens::open_at(*d, root, longtext, Right::Read), Status::InvalidPath, "text over 512 bytes");
    char deep[200];
    int n = 0;
    for (int i = 0; i < 33; ++i) { deep[n++] = 'd'; deep[n++] = '/'; }
    deep[n] = 0;
    CHECK_L(lens::open_at(*d, root, deep, Right::Read), Status::InvalidPath, "33 components");
    CHECK_L(lens::open_at(*d, Lens(), "x", Right::Read), Status::BadLens, "null lens");

    lens::close_all(*d);
    free(d);
}

static void test_rights_flow() {
    Domain* d = new_domain();
    lens::test_unseal();

    bool exact = true;
    u32 wrongly_open = 0, wrongly_denied = 0;
    for (u32 pm = 0; pm < 512; ++pm) {
        Lens parent = must_mint(*d, "ram:/proj", Rights::from_bits(pm));
        for (u32 cm = 0; cm < 512; ++cm) {
            Rights child = Rights::from_bits(cm);
            auto r = lens::open_at(*d, parent, ".", child);
            bool want = pm != 0 && child.subset_of(Rights::from_bits(pm));
            if (r.ok() != want) { exact = false; if (r.ok()) ++wrongly_open; else ++wrongly_denied; }
            if (r.ok()) {
                if (lens::rights_of(*d, r.value()).value() != child) exact = false;
                (void)lens::close(*d, r.value());
            } else if (r.status() != Status::Denied) {
                exact = false;
            }
        }
        (void)lens::close(*d, parent);
    }
    CHECK(exact, "open_at grants exactly the subsets of the parent's rights");
    CHECK(wrongly_open == 0 && wrongly_denied == 0, "no escalation and no false denial");
    CHECK(lens::live_count(*d) == 0, "exhaustive pass leaked no slots");

    bool weak = true;
    for (u32 a = 0; a < 512; a += 7) {
        Lens l = must_mint(*d, "ram:/proj", Rights::from_bits(a));
        for (u32 k = 0; k < 512; ++k) {
            auto r = lens::narrow(*d, l, Rights::from_bits(k));
            if (!r.ok()) { weak = false; continue; }
            Rights got = lens::rights_of(*d, r.value()).value();
            if (got != (Rights::from_bits(a) & Rights::from_bits(k)) || !got.subset_of(Rights::from_bits(a))) weak = false;
            (void)lens::close(*d, r.value());
        }
        (void)lens::close(*d, l);
    }
    CHECK(weak, "narrow only ever weakens");

    Lens full = must_mint(*d, "ram:/proj", ALL);
    Lens narrowed = lens::narrow(*d, full, Right::Read | Right::Enumerate).value();
    auto widen = lens::narrow(*d, narrowed, ALL);
    CHECK(widen.ok() && lens::rights_of(*d, widen.value()).value() == (Right::Read | Right::Enumerate), "narrowing again cannot widen");
    CHECK(lens::rights_of(*d, full).value() == ALL, "narrow leaves the source lens alone");
    auto widen2 = lens::open_at(*d, narrowed, ".", Right::Read | Right::Write);
    CHECK(widen2.status() == Status::Denied, "open_at cannot widen a narrowed lens");

    Lens ro = must_mint(*d, "ram:/proj", ALL, true);
    CHECK(lens::read_only_of(*d, ro).value(), "mint can be read-only");
    CHECK(lens::read_only_of(*d, lens::narrow(*d, ro, ALL, false).value()).value(), "narrow(false) cannot clear read-only");
    CHECK(lens::read_only_of(*d, must_open(*d, ro, "src", ALL)).value(), "children inherit read-only");
    CHECK(!lens::read_only_of(*d, full).value(), "plain lens is writable");
    CHECK(lens::read_only_of(*d, lens::narrow(*d, full, ALL, true).value()).value(), "narrow(true) sets read-only");

    Lens none = must_mint(*d, "ram:/proj", Rights::none());
    CHECK_L(lens::open_at(*d, none, "src", Rights::none()), Status::Denied, "no rights, no lookup");
    CHECK_L(lens::open_at(*d, none, ".", Rights::none()), Status::Denied, "no rights, no copy either");

    lens::close_all(*d);
    free(d);
}

static void test_delegate() {
    Domain* a = new_domain();
    Domain* b = new_domain();
    lens::test_unseal();

    Lens owner = must_mint(*a, "ram:/proj", Right::Read | Right::Write | Right::Enumerate | Right::Delegate);
    Lens plain = must_mint(*a, "ram:/proj", Right::Read | Right::Write | Right::Enumerate);

    CHECK_L(lens::delegate(*a, plain, *b, Right::Read), Status::Denied, "delegate without the Delegate right");
    CHECK_L(lens::delegate(*a, plain, *b, Rights::none()), Status::Denied, "delegate without the right, even nothing");
    CHECK(lens::live_count(*b) == 0, "refused delegation allocated nothing");

    CHECK_L(lens::delegate(*a, owner, *b, Right::Read | Right::Append), Status::Denied, "cannot hand over a right you lack");
    CHECK_L(lens::delegate(*a, owner, *b, ALL), Status::Denied, "cannot hand over everything");
    CHECK_L(lens::delegate(*a, owner, *b, Right::Read | Right::Freeze), Status::Denied, "one extra bit is enough to refuse");

    auto ok = lens::delegate(*a, owner, *b, Right::Read | Right::Enumerate);
    CHECK(ok.ok(), "delegate a subset");
    Lens in_b = ok.value();
    CHECK(lens::rights_of(*b, in_b).value() == (Right::Read | Right::Enumerate), "delegated rights exact");
    CHECK(!lens::rights_of(*b, in_b).value().has(Right::Delegate), "Delegate is not passed on unless asked for");
    CHECK(lens::stat(*b, in_b).ok(), "delegated lens works in the other domain");
    CHECK_L(lens::delegate(*b, in_b, *a, Right::Read), Status::Denied, "the receiver cannot delegate onward");

    auto with = lens::delegate(*a, owner, *b, Right::Read | Right::Delegate);
    CHECK(with.ok(), "Delegate can be passed on when asked for");
    CHECK(lens::delegate(*b, with.value(), *a, Right::Read).ok(), "and then used");
    CHECK_L(lens::delegate(*b, with.value(), *a, Right::Read | Right::Write), Status::Denied, "but never beyond what it holds");

    Lens src = must_open(*a, owner, "src", Right::Read | Right::Delegate);
    Lens src_b = lens::delegate(*a, src, *b, Right::Read).value();
    CHECK(lens::depth_of(*b, src_b).value() == 0, "delegated lens starts a new family at depth 0");
    lens::Slot* sa = lens::slot_of(*a, src).value();
    lens::Slot* sb = lens::slot_of(*b, src_b).value();
    CHECK(sb->root.id == sb->node.id && sa->root.id != sa->node.id, "root bound moves to the delegated node");
    CHECK(sb->node.id == sa->node.id, "same node underneath");

    Lens rol = must_mint(*a, "ram:/proj", Right::Read | Right::Delegate, true);
    CHECK(lens::read_only_of(*b, lens::delegate(*a, rol, *b, Right::Read).value()).value(), "read-only is inherited by delegation");

    CHECK(lens::delegate(*a, owner, *a, Right::Read).ok(), "delegate within one domain");

    lens::close_all(*a);
    lens::close_all(*b);
    free(a);
    free(b);
}

static void test_stale() {
    Domain* d = new_domain();
    lens::test_unseal();
    Lens root = must_mint(*d, "ram:/proj", ALL);
    Lens child = must_open(*d, root, "src", ALL);

    CHECK(lens::close(*d, child).ok(), "close");
    CHECK_L(lens::stat(*d, child), Status::StaleLens, "stat after close");
    CHECK_L(lens::open_at(*d, child, ".", Right::Read), Status::StaleLens, "open_at after close");
    CHECK_L(lens::narrow(*d, child, ALL), Status::StaleLens, "narrow after close");
    CHECK_L(lens::delegate(*d, child, *d, Right::Read), Status::StaleLens, "delegate after close");
    CHECK_L(lens::rights_of(*d, child), Status::StaleLens, "rights_of after close");
    CHECK_L(lens::close(*d, child), Status::StaleLens, "double close");

    Lens reuse = must_open(*d, root, "src", ALL);
    CHECK((reuse.raw & 0xFFFFFFFFull) == (child.raw & 0xFFFFFFFFull), "freed slot is reused");
    CHECK((reuse.raw >> 32) != (child.raw >> 32), "with a new generation");
    CHECK_L(lens::stat(*d, child), Status::StaleLens, "old handle still stale after reuse");
    CHECK(lens::stat(*d, reuse).ok(), "new handle works");

    Lens grand = must_open(*d, reuse, "deep", ALL);
    CHECK(lens::close(*d, reuse).ok(), "close the parent");
    CHECK(lens::stat(*d, grand).ok(), "child outlives the parent");
    CHECK(lens::open_at(*d, grand, "leaf.txt", Right::Read).ok(), "and can still derive");

    CHECK_L(lens::stat(*d, Lens()), Status::BadLens, "null handle");
    CHECK_L(lens::stat(*d, Lens(0x100000000ull)), Status::BadLens, "slot index zero");
    CHECK_L(lens::stat(*d, Lens(0x1FFFFFFFFull)), Status::BadLens, "slot index out of range");
    CHECK_L(lens::stat(*d, Lens(0x100000000ull | (lens::MAX_LENSES + 1))), Status::BadLens, "just past the table");
    CHECK_L(lens::stat(*d, Lens((0xFFFFull << 32) | 3)), Status::StaleLens, "unused slot");
    CHECK_L(lens::stat(*d, Lens((0x7777ull << 32) | (root.raw & 0xFFFFFFFFull))), Status::StaleLens, "wrong generation on a live slot");

    Domain* other = new_domain();
    Lens foreign = must_mint(*other, "ram:/proj", ALL);
    CHECK(!lens::is_live(*d, foreign) || (foreign.raw & 0xFFFFFFFFull) == (root.raw & 0xFFFFFFFFull), "foreign handle is not accepted by index alone");
    lens::close_all(*other);
    free(other);

    lens::close_all(*d);
    CHECK(lens::live_count(*d) == 0, "close_all empties the domain");
    free(d);
}

static void test_table_limits() {
    Domain* d = new_domain();
    lens::test_unseal();
    Lens v[lens::MAX_LENSES];
    for (u32 i = 0; i < lens::MAX_LENSES; ++i) v[i] = must_mint(*d, "ram:/proj", ALL);
    CHECK(lens::live_count(*d) == lens::MAX_LENSES, "table filled");
    CHECK_L(lens::mint(*d, "ram:/proj", ALL), Status::TableFull, "mint on a full table");
    CHECK_L(lens::open_at(*d, v[0], ".", Right::Read), Status::TableFull, "open_at on a full table");
    CHECK_L(lens::narrow(*d, v[0], Right::Read), Status::TableFull, "narrow on a full table");
    CHECK(lens::close(*d, v[10]).ok(), "free one");
    CHECK(lens::open_at(*d, v[0], ".", Right::Read).ok(), "and one more fits");
    CHECK(lens::live_count(*d) == lens::MAX_LENSES, "back to full");

    lens::close_all(*d);
    bool nonzero = true;
    Lens prev;
    for (u32 i = 0; i < 70000; ++i) {
        auto m = lens::mint(*d, "ram:/", Right::Read);
        if (!m.ok() || m.value().raw == 0 || (m.value().raw >> 32) == 0) { nonzero = false; break; }
        if (i && m.value() == prev) nonzero = false;
        prev = m.value();
        (void)lens::close(*d, m.value());
    }
    CHECK(nonzero, "70000 reuses of a slot: handle never zero, generation never zero, never repeats");

    lens::close_all(*d);
    Lens held = must_mint(*d, "ram:/proj", ALL);
    u32 index = static_cast<u32>(held.raw & 0xFFFFFFFFull) - 1;
    CHECK(lens::close(*d, held).ok(), "close the lens whose slot is about to run out");
    d->slots[index].generation = 0xFFFFFFFEu;
    Lens last = must_mint(*d, "ram:/proj", ALL);
    CHECK(static_cast<u32>(last.raw & 0xFFFFFFFFull) - 1 == index && (last.raw >> 32) == 0xFFFFFFFFull, "the slot's last generation is handed out once");
    CHECK(lens::close(*d, last).ok(), "closing it retires the slot");
    CHECK(d->slots[index].retired, "the slot is retired instead of wrapping");
    bool reused = false;
    for (u32 i = 0; i < lens::MAX_LENSES - 1; ++i) {
        Lens x = must_mint(*d, "ram:/proj", ALL);
        if (static_cast<u32>(x.raw & 0xFFFFFFFFull) - 1 == index) reused = true;
    }
    CHECK_L(lens::mint(*d, "ram:/proj", ALL), Status::TableFull, "and the table reports full without it");
    CHECK(!reused, "a retired slot is never handed out again");
    CHECK_L(lens::stat(*d, last), Status::StaleLens, "the last handle stays stale forever");
    CHECK(lens::live_count(*d) == lens::MAX_LENSES - 1, "the table is one slot smaller");
    lens::close_all(*d);

    free(d);
}

static void test_zero_memory_domain() {
    static Domain g_zero;
    lens::test_unseal();
    CHECK(g_zero.live == 0, "zero domain is empty");
    Lens l = must_mint(g_zero, "ram:/", ALL);
    CHECK(lens::stat(g_zero, l).ok(), "zero domain works without construction");
    lens::close_all(g_zero);
    CHECK(g_zero.live == 0, "and returns to empty");
}

static bool content_is(const char* path, const char* want) {
    nebula::Blob b;
    if (nebula::read_file(g_sess, path, &b) != nebula::Status::Ok) return false;
    bool ok = b.size == cstr::length(want) && (b.size == 0 || mem::compare(b.data, want, b.size) == 0);
    nebula::release(b);
    return ok;
}

static bool present(const char* path) {
    nebula::Info inf;
    return nebula::stat(g_sess, path, &inf) == nebula::Status::Ok;
}

static lens::ConstBytes txt(const char* s) { return lens::text_bytes(s); }

static bool read_text(Domain& d, Lens l, u64 off, char* out, usize cap) {
    auto r = lens::read(d, l, off, lens::Bytes(reinterpret_cast<u8*>(out), cap - 1));
    if (!r.ok()) return false;
    out[r.value()] = 0;
    return true;
}

struct Names {
    char n[512][nebula::NAME_BYTES + 1];
    u32 count;
};
static bool collect_name(void* ctx, const nebula::Entry& e) {
    Names* v = static_cast<Names*>(ctx);
    if (v->count < 512) { mem::copy(v->n[v->count], e.name, sizeof(e.name)); ++v->count; }
    return true;
}
static bool has_name(const Names& v, const char* name) {
    for (u32 i = 0; i < v.count; ++i) if (same(v.n[i], name)) return true;
    return false;
}
struct Stop { u32 seen; u32 stop_after; };
static bool stop_visitor(void* ctx, const nebula::Entry&) {
    Stop* s = static_cast<Stop*>(ctx);
    return ++s->seen < s->stop_after;
}
struct SizeSum { u64 total; u32 files; };
static bool size_visitor(void* ctx, const nebula::Entry& e) {
    SizeSum* s = static_cast<SizeSum*>(ctx);
    if (e.kind == nebula::Kind::File) { s->total += e.size; ++s->files; }
    return true;
}

static void ops_data(const char* realm, Domain& d, Lens root) {
    char b[160];
    auto P = [&](const char* rel) { __builtin_snprintf(b, sizeof(b), "%s:/proj/%s", realm, rel); return b; };

    Lens rd = must_open(d, root, "readme.txt", Right::Read);
    char buf[64];
    CHECK(read_text(d, rd, 0, buf, sizeof(buf)) && same(buf, "hello lens"), "read whole file");
    CHECK(read_text(d, rd, 6, buf, sizeof(buf)) && same(buf, "lens"), "read from an offset");
    CHECK(read_text(d, rd, 10, buf, sizeof(buf)) && same(buf, ""), "read at end gives nothing");
    CHECK(read_text(d, rd, 9999, buf, sizeof(buf)) && same(buf, ""), "read past end gives nothing");
    auto few = lens::read(d, rd, 0, lens::Bytes(reinterpret_cast<u8*>(buf), 4));
    CHECK(few.ok() && few.value() == 4 && mem::compare(buf, "hell", 4) == 0, "short destination gets a short read");
    CHECK(lens::read(d, rd, 0, lens::Bytes()).value() == 0, "empty destination reads zero");
    CHECK(lens::size(d, rd).value() == 10, "size");

    CHECK_L(lens::write(d, rd, 0, txt("x")), Status::Denied, "write without the right");
    CHECK_L(lens::append(d, rd, txt("x")), Status::Denied, "append without the right");
    CHECK_L(lens::resize(d, rd, 0), Status::Denied, "resize without the right");
    CHECK_L(lens::sync(d, rd), Status::Denied, "sync without the right");
    CHECK(content_is(P("readme.txt"), "hello lens"), "denied operations changed nothing");
    Lens wo = must_open(d, root, "readme.txt", Right::Write);
    CHECK_L(lens::read(d, wo, 0, lens::Bytes(reinterpret_cast<u8*>(buf), 8)), Status::Denied, "read without the right");
    Lens folder = must_open(d, root, "src", ALL);
    CHECK_L(lens::read(d, folder, 0, lens::Bytes(reinterpret_cast<u8*>(buf), 8)), Status::IsADirectory, "read a folder");
    CHECK_L(lens::write(d, folder, 0, txt("x")), Status::IsADirectory, "write a folder");
    CHECK_L(lens::enumerate(d, rd, &collect_name, nullptr), Status::NotADirectory, "enumerate a blob");
    CHECK_L(lens::read(d, Lens(), 0, lens::Bytes(reinterpret_cast<u8*>(buf), 8)), Status::BadLens, "read through a null lens");

    Lens rw = must_open(d, root, "readme.txt", Right::Read | Right::Write | Right::Append);
    CHECK(lens::write(d, rw, 6, txt("LENS")).value() == 4, "write returns the byte count");
    CHECK(read_text(d, rw, 0, buf, sizeof(buf)) && same(buf, "hello LENS"), "the writing lens sees its own bytes");
    CHECK(content_is(P("readme.txt"), "hello lens"), "unsynced bytes are not in the realm yet");
    CHECK(lens::sync(d, rw).ok(), "sync");
    CHECK(content_is(P("readme.txt"), "hello LENS"), "synced bytes are in the realm");
    CHECK(lens::append(d, rw, txt("!!")).ok(), "append");
    CHECK(lens::size(d, rw).value() == 12, "size includes unsynced append");
    CHECK(lens::write(d, rw, 14, txt("Z")).ok(), "write past the end");
    CHECK(read_text(d, rw, 12, buf, sizeof(buf)) && buf[0] == 0 && buf[1] == 0 && buf[2] == 'Z', "the gap is zero-filled");
    CHECK(lens::resize(d, rw, 5).ok(), "shrink");
    CHECK(read_text(d, rw, 0, buf, sizeof(buf)) && same(buf, "hello"), "shrunk content");
    CHECK(lens::resize(d, rw, 8).ok() && lens::size(d, rw).value() == 8, "grow");
    CHECK(lens::resize(d, rw, 5).ok() && lens::sync(d, rw).ok(), "and back");
    CHECK(content_is(P("readme.txt"), "hello"), "resize written back");
    CHECK(lens::write(d, rw, 0, lens::ConstBytes()).ok(), "zero-length write is fine");
    CHECK_L(lens::write(d, rw, nebula::MAX_FILE_BYTES, txt("x")), Status::TooLarge, "write beyond 16 MiB");
    CHECK_L(lens::resize(d, rw, nebula::MAX_FILE_BYTES + 1), Status::TooLarge, "resize beyond 16 MiB");
    CHECK(lens::sync(d, rw).ok() && lens::sync(d, rw).ok(), "sync twice");

    Lens rw2 = must_open(d, root, "readme.txt", Right::Write);
    CHECK_L(lens::write(d, rw2, 0, txt("x")), Status::Busy, "second writer on the same file");
    CHECK(read_text(d, rd, 0, buf, sizeof(buf)), "a reader still reads");
    CHECK(lens::close(d, rw).ok(), "close the writer");
    CHECK(lens::write(d, rw2, 0, txt("J")).ok(), "now the second lens can write");
    CHECK(lens::close(d, rw2).ok(), "close writes back");
    CHECK(content_is(P("readme.txt"), "Jello"), "close wrote the buffer");
    CHECK(lens::close(d, wo).ok() && lens::close(d, rd).ok(), "close readers");

    Lens rw3 = must_open(d, root, "readme.txt", Right::Read | Right::Write);
    CHECK(read_text(d, rw3, 0, buf, sizeof(buf)) && same(buf, "Jello"), "first touch is a read");
    CHECK(lens::write(d, rw3, 0, txt("Y")).ok() && read_text(d, rw3, 0, buf, sizeof(buf)) && same(buf, "Yello"), "then a write upgrades the buffer");
    CHECK(lens::close(d, rw3).ok(), "close");
    CHECK(content_is(P("readme.txt"), "Yello"), "upgrade wrote back");

    Lens ap = must_open(d, root, "readme.txt", Right::Append);
    CHECK(lens::append(d, ap, txt("+")).ok(), "append-only lens appends");
    CHECK_L(lens::write(d, ap, 0, txt("x")), Status::Denied, "append-only lens cannot write");
    CHECK_L(lens::read(d, ap, 0, lens::Bytes(reinterpret_cast<u8*>(buf), 8)), Status::Denied, "append-only lens cannot read");
    CHECK_L(lens::resize(d, ap, 0), Status::Denied, "append-only lens cannot resize");
    CHECK(lens::sync(d, ap).ok(), "append-only lens can sync");
    CHECK(content_is(P("readme.txt"), "Yello+"), "appended");
    lens::close(d, ap);

    Lens ro = lens::narrow(d, root, ALL, true).value();
    Lens rof = must_open(d, ro, "readme.txt", ALL);
    CHECK_L(lens::write(d, rof, 0, txt("x")), Status::ReadOnly, "write through a read-only lens");
    CHECK_L(lens::append(d, rof, txt("x")), Status::ReadOnly, "append through a read-only lens");
    CHECK_L(lens::resize(d, rof, 0), Status::ReadOnly, "resize through a read-only lens");
    CHECK(read_text(d, rof, 0, buf, sizeof(buf)) && same(buf, "Yello+"), "but it reads");
    CHECK(content_is(P("readme.txt"), "Yello+"), "nothing changed");
    CHECK_L(lens::create(d, ro, "newfile", Right::Read), Status::ReadOnly, "create through a read-only lens");
    CHECK_L(lens::mkdir(d, ro, "newdir"), Status::ReadOnly, "mkdir through a read-only lens");
    CHECK_L(lens::unlink(d, ro, "readme.txt"), Status::ReadOnly, "unlink through a read-only lens");
    CHECK_L(lens::rename(d, ro, "readme.txt", ro, "r2"), Status::ReadOnly, "rename through a read-only lens");
    CHECK_L(lens::copy(d, rof, ro, "c"), Status::ReadOnly, "copy into a read-only lens");
    CHECK(!present(P("newfile")) && !present(P("newdir")) && !present(P("r2")) && !present(P("c")), "no read-only attempt left anything behind");
    lens::close(d, rof);
    lens::close(d, ro);

    nebula::write_file(g_sess, P("readme.txt"), "hello lens", 10);
    lens::close(d, folder);
    lens::close(d, rd);
}

static void ops_dirs(const char* realm, Domain& d, Lens root) {
    char b[160];
    auto P = [&](const char* rel) { __builtin_snprintf(b, sizeof(b), "%s:/proj/%s", realm, rel); return b; };

    Names v{};
    auto en = lens::enumerate(d, root, &collect_name, &v);
    CHECK(en.ok() && en.value().visited == v.count && !en.value().truncated, "enumerate visits everything");
    CHECK(v.count == 3 && has_name(v, "src") && has_name(v, "readme.txt") && has_name(v, "a:b"), "enumerate names");
    Stop stop{0, 2};
    CHECK(lens::enumerate(d, root, &stop_visitor, &stop).value().visited == 2 && stop.seen == 2, "visitor can stop early");
    SizeSum ss{};
    CHECK(lens::enumerate(d, root, &size_visitor, &ss, true).ok() && ss.files == 2 && ss.total == 10 + 10, "with_info fills sizes");
    SizeSum ss0{};
    lens::enumerate(d, root, &size_visitor, &ss0, false);
    CHECK(ss0.total == 0, "without with_info the sizes stay zero");
    Lens noenum = lens::narrow(d, root, Right::Read).value();
    CHECK_L(lens::enumerate(d, noenum, &collect_name, &v), Status::Denied, "enumerate needs the right");
    lens::close(d, noenum);

    CHECK(lens::mkdir(d, root, "big").ok(), "mkdir big");
    Lens big = must_open(d, root, "big", ALL);
    char name[16];
    bool all_made = true;
    for (int i = 0; i < 300; ++i) {
        __builtin_snprintf(name, sizeof(name), "f%03d", i);
        auto c = lens::create(d, big, name, Rights::none());
        if (!c.ok()) { all_made = false; break; }
        lens::close(d, c.value());
    }
    CHECK(all_made, "300 creates");
    u32 count = 0;
    struct Count { u32* n; };
    Count cc{&count};
    auto counter = [](void* ctx, const nebula::Entry&) { ++*static_cast<Count*>(ctx)->n; return true; };
    CHECK(lens::enumerate(d, big, counter, &cc).value().visited == 300 && count == 300, "all 300 names visited");
    CHECK(lens::remove_tree(d, root, "big").ok(), "remove the big directory");
    lens::close(d, big);

    Lens nf = lens::create(d, root, "made.txt", Right::Read | Right::Write).value();
    CHECK(!nf.is_null() && present(P("made.txt")), "create makes the file");
    CHECK(lens::kind_of(d, nf).value() == NodeKind::Blob && lens::size(d, nf).value() == 0, "an empty blob");
    CHECK(lens::rights_of(d, nf).value() == (Right::Read | Right::Write), "with the requested rights");
    CHECK(lens::write(d, nf, 0, txt("fresh")).ok() && lens::close(d, nf).ok(), "write to the new file");
    CHECK(content_is(P("made.txt"), "fresh"), "and it landed");
    CHECK_L(lens::create(d, root, "made.txt", Right::Read), Status::Exists, "create over an existing file");
    CHECK_L(lens::create(d, root, "src", Right::Read), Status::Exists, "create over a folder");
    CHECK_L(lens::create(d, root, "nodir/x", Right::Read), Status::NotFound, "create under a missing folder");
    CHECK_L(lens::create(d, root, "readme.txt/x", Right::Read), Status::NotADirectory, "create under a file");
    CHECK_L(lens::create(d, root, "../x", Right::Read), Status::Escape, "create refuses dotdot");
    CHECK_L(lens::create(d, root, "/x", Right::Read), Status::Escape, "create refuses absolute");
    CHECK_L(lens::create(d, root, "ram:/x", Right::Read), Status::Escape, "create refuses a realm prefix");
    CHECK_L(lens::create(d, root, "", Right::Read), Status::InvalidPath, "create with an empty path");
    CHECK_L(lens::create(d, root, ".", Right::Read), Status::InvalidPath, "create with no leaf");
    CHECK_L(lens::create(d, root, "bad\x01name", Right::Read), Status::InvalidName, "create with a bad name");
    Lens deepc = lens::create(d, root, "src/deep/new.txt", Right::Read).value();
    CHECK(!deepc.is_null() && present(P("src/deep/new.txt")), "create at a nested path");
    CHECK(lens::depth_of(d, deepc).value() == 3, "with the right depth");
    lens::close(d, deepc);
    Lens limited = lens::narrow(d, root, Right::Create | Right::Read).value();
    CHECK_L(lens::create(d, limited, "wide", Right::Read | Right::Write), Status::Denied, "created lens cannot exceed the folder's rights");
    CHECK(!present(P("wide")), "and nothing was created");
    Lens nocreate = lens::narrow(d, root, Right::Read | Right::Write).value();
    CHECK_L(lens::create(d, nocreate, "x2", Right::Read), Status::Denied, "create needs the right");
    CHECK_L(lens::mkdir(d, nocreate, "x2"), Status::Denied, "mkdir needs the right");
    lens::close(d, limited);
    lens::close(d, nocreate);

    {
        Domain* full = new_domain();
        Lens r2 = must_mint(*full, (__builtin_snprintf(b, sizeof(b), "%s:/proj", realm), b), ALL);
        Lens fill[lens::MAX_LENSES];
        u32 n = 1;
        fill[0] = r2;
        while (n < lens::MAX_LENSES) fill[n++] = must_mint(*full, (__builtin_snprintf(b, sizeof(b), "%s:/proj", realm), b), ALL);
        CHECK_L(lens::create(*full, r2, "ghost", Right::Read), Status::TableFull, "create with no free slot");
        CHECK(!present(P("ghost")), "no ghost file after TableFull");
        lens::close_all(*full);
        free(full);
    }
    lens::unlink(d, root, "made.txt");
    lens::unlink(d, root, "src/deep/new.txt");

    CHECK(lens::mkdir(d, root, "newdir").ok() && present(P("newdir")), "mkdir");
    CHECK_L(lens::mkdir(d, root, "newdir"), Status::Exists, "mkdir twice");
    CHECK_L(lens::mkdir(d, root, "a/b/c"), Status::NotFound, "mkdir without parents");
    CHECK(lens::mkdir(d, root, "a/b/c", true).ok() && present(P("a/b/c")), "mkdir with parents");
    CHECK(lens::mkdir(d, root, "a/b/c", true).ok(), "mkdir -p over an existing folder is fine");
    CHECK_L(lens::mkdir(d, root, "readme.txt", true), Status::NotADirectory, "mkdir -p over a file");
    CHECK_L(lens::mkdir(d, root, "readme.txt/x", true), Status::NotADirectory, "mkdir -p through a file");
    CHECK_L(lens::mkdir(d, root, "../up"), Status::Escape, "mkdir refuses dotdot");
    CHECK_L(lens::mkdir(d, root, ""), Status::InvalidPath, "mkdir empty");
    CHECK_L(lens::mkdir(d, root, "."), Status::InvalidPath, "mkdir dot");

    CHECK_L(lens::unlink(d, root, "a"), Status::NotEmpty, "unlink a non-empty folder");
    CHECK(lens::unlink(d, root, "newdir").ok() && !present(P("newdir")), "unlink an empty folder");
    CHECK_L(lens::unlink(d, root, "newdir"), Status::NotFound, "unlink twice");
    CHECK_L(lens::unlink(d, root, "../x"), Status::Escape, "unlink refuses dotdot");
    CHECK_L(lens::unlink(d, root, "a/b/nothing"), Status::NotFound, "unlink missing in a nested path");
    u32 removed = 0;
    CHECK(lens::remove_tree(d, root, "a", &removed).ok() && removed == 3 && !present(P("a")), "remove_tree counts and removes");
    CHECK_L(lens::remove_tree(d, root, "a"), Status::NotFound, "remove_tree missing");
    Lens nounlink = lens::narrow(d, root, ALL & ~Rights(Right::Unlink)).value();
    CHECK_L(lens::unlink(d, nounlink, "readme.txt"), Status::Denied, "unlink needs the right");
    CHECK_L(lens::remove_tree(d, nounlink, "src"), Status::Denied, "remove_tree needs the right");
    CHECK(present(P("readme.txt")) && present(P("src/main.c")), "denied removals changed nothing");
    lens::close(d, nounlink);

    Lens busy = must_open(d, root, "readme.txt", Right::Write);
    CHECK(lens::write(d, busy, 0, txt("h")).ok(), "claim the writer slot");
    CHECK_L(lens::unlink(d, root, "readme.txt"), Status::Busy, "unlink a file that is being written");
    CHECK_L(lens::rename(d, root, "readme.txt", root, "moved.txt"), Status::Busy, "rename a file that is being written");
    CHECK_L(lens::remove_tree(d, root, "readme.txt"), Status::Busy, "remove_tree on a busy file");
    lens::close(d, busy);
    CHECK(lens::unlink(d, root, "readme.txt").ok(), "after close it can go");
    put(P("readme.txt"), "hello lens");
}

static void ops_rename_copy(const char* realm, Domain& d, Lens root) {
    char b[160];
    auto P = [&](const char* rel) { __builtin_snprintf(b, sizeof(b), "%s:/proj/%s", realm, rel); return b; };

    CHECK(lens::rename(d, root, "readme.txt", root, "moved.txt").ok(), "rename in one folder");
    CHECK(!present(P("readme.txt")) && content_is(P("moved.txt"), "hello lens"), "moved");
    Lens src = must_open(d, root, "src", ALL);
    Lens deep = must_open(d, root, "src/deep", ALL);
    CHECK(lens::rename(d, root, "moved.txt", deep, "m.txt").ok(), "rename across folders with two lenses");
    CHECK(content_is(P("src/deep/m.txt"), "hello lens"), "arrived");
    CHECK(lens::rename(d, src, "deep/m.txt", root, "readme.txt").ok(), "rename back up using a relative path");
    CHECK(content_is(P("readme.txt"), "hello lens"), "restored");
    CHECK_L(lens::rename(d, root, "nothing", root, "x"), Status::NotFound, "rename a missing name");
    CHECK_L(lens::rename(d, root, "readme.txt", root, "src/main.c"), Status::Exists, "rename onto an existing name");
    CHECK(content_is(P("src/main.c"), "int main;"), "the target survived");
    CHECK(lens::rename(d, root, "readme.txt", root, "src/main.c", true).ok(), "rename with replace");
    CHECK(content_is(P("src/main.c"), "hello lens") && !present(P("readme.txt")), "replaced");
    put(P("readme.txt"), "hello lens");
    put(P("src/main.c"), "int main;");
    CHECK(lens::rename(d, root, "readme.txt", root, "./readme.txt").ok(), "rename onto itself is a no-op");
    CHECK(present(P("readme.txt")), "and the file is still there");
    CHECK_L(lens::rename(d, root, "../x", root, "y"), Status::Escape, "rename refuses dotdot on the source");
    CHECK_L(lens::rename(d, root, "readme.txt", root, "../y"), Status::Escape, "rename refuses dotdot on the target");
    CHECK_L(lens::rename(d, root, "readme.txt", root, "disk:/y"), Status::Escape, "rename refuses a realm prefix");
    CHECK_L(lens::rename(d, root, "src", deep, "src2"), Status::Unsupported, "a folder cannot move below itself");
    CHECK_L(lens::rename(d, root, "src", root, "src/inner"), Status::Unsupported, "nor to a path inside itself");
    CHECK(present(P("src/main.c")), "the failed folder move changed nothing");
    CHECK(lens::rename(d, root, "src/deep", root, "deep2").ok() && present(P("deep2/leaf.txt")), "a folder moves up");
    CHECK(lens::rename(d, root, "deep2", root, "src/deep").ok() && present(P("src/deep/leaf.txt")), "and back");

    Lens rn = lens::narrow(d, root, Right::Rename).value();
    Lens rn_unlink = lens::narrow(d, root, Right::Rename | Right::Unlink).value();
    Lens norn = lens::narrow(d, root, ALL & ~Rights(Right::Rename)).value();
    CHECK_L(lens::rename(d, norn, "readme.txt", root, "z"), Status::Denied, "source lens needs Rename");
    CHECK_L(lens::rename(d, root, "readme.txt", norn, "z"), Status::Denied, "target lens needs Rename");
    CHECK_L(lens::rename(d, rn, "readme.txt", rn, "src/main.c", true), Status::Denied, "replacing needs Unlink on the target lens");
    CHECK(lens::rename(d, rn, "readme.txt", rn, "z").ok(), "Rename alone is enough for a plain move");
    CHECK(lens::rename(d, rn, "z", rn, "readme.txt").ok(), "and back");
    CHECK(lens::rename(d, rn_unlink, "readme.txt", rn_unlink, "src/main.c", true).ok(), "Rename and Unlink replace");
    put(P("readme.txt"), "hello lens");
    put(P("src/main.c"), "int main;");
    lens::close(d, rn); lens::close(d, rn_unlink); lens::close(d, norn);

    Lens f = must_open(d, root, "readme.txt", Right::Read);
    CHECK(lens::copy(d, f, root, "copy.txt").ok() && content_is(P("copy.txt"), "hello lens"), "copy to a new file");
    put(P("copy.txt"), "old");
    CHECK(lens::copy(d, f, root, "copy.txt").ok() && content_is(P("copy.txt"), "hello lens"), "copy overwrites");
    CHECK(lens::copy(d, f, deep, "c2.txt").ok() && content_is(P("src/deep/c2.txt"), "hello lens"), "copy into another folder lens");
    CHECK(lens::copy(d, f, root, "src/c3.txt").ok() && present(P("src/c3.txt")), "copy to a nested path");
    CHECK_L(lens::copy(d, f, root, "readme.txt"), Status::Exists, "copy onto itself");
    CHECK_L(lens::copy(d, f, root, "src"), Status::IsADirectory, "copy onto a folder");
    CHECK_L(lens::copy(d, f, root, "nodir/x"), Status::NotFound, "copy under a missing folder");
    CHECK_L(lens::copy(d, f, root, "../x"), Status::Escape, "copy refuses dotdot");
    CHECK_L(lens::copy(d, src, root, "x"), Status::IsADirectory, "copy from a folder lens");
    Lens nord = lens::narrow(d, root, ALL & ~Rights(Right::Read)).value();
    Lens nof = must_open(d, nord, "readme.txt", Right::Write);
    CHECK_L(lens::copy(d, nof, root, "q"), Status::Denied, "copy needs Read on the source");
    Lens nocr = lens::narrow(d, root, Right::Read | Right::Write).value();
    CHECK_L(lens::copy(d, f, nocr, "newcopy"), Status::Denied, "copy to a new name needs Create");
    CHECK(lens::copy(d, f, nocr, "copy.txt").ok(), "copy over an existing file needs only Write");
    Lens nowr = lens::narrow(d, root, Right::Read | Right::Create).value();
    CHECK_L(lens::copy(d, f, nowr, "copy.txt"), Status::Denied, "overwriting needs Write");
    CHECK(!present(P("newcopy")) && !present(P("q")), "denied copies left nothing");
    lens::close(d, nord); lens::close(d, nof); lens::close(d, nocr); lens::close(d, nowr);
    lens::unlink(d, root, "copy.txt");
    lens::unlink(d, root, "src/deep/c2.txt");
    lens::unlink(d, root, "src/c3.txt");

    u32 copied = 0;
    CHECK(lens::copy_tree(d, src, root, "srccopy", &copied).ok() && copied == 2, "copy_tree counts files");
    CHECK(content_is(P("srccopy/main.c"), "int main;") && content_is(P("srccopy/deep/leaf.txt"), "leaf"), "copy_tree contents");
    CHECK(lens::copy_tree(d, src, root, "srccopy", &copied).ok() && copied == 2, "copy_tree over an existing tree merges");
    CHECK_L(lens::copy_tree(d, src, src, "inside"), Status::Unsupported, "copy_tree into itself");
    CHECK_L(lens::copy_tree(d, src, deep, "inside"), Status::Unsupported, "copy_tree below itself");
    CHECK_L(lens::copy_tree(d, src, root, "../x"), Status::Escape, "copy_tree refuses dotdot");
    CHECK_L(lens::copy_tree(d, src, root, "readme.txt"), Status::NotADirectory, "copy_tree onto a file");
    Lens noenum = lens::narrow(d, root, Right::Read | Right::Create).value();
    Lens src_ne = must_open(d, noenum, "src", Right::Read);
    CHECK_L(lens::copy_tree(d, src_ne, root, "z"), Status::Denied, "copy_tree needs Enumerate on the source");
    lens::close(d, noenum); lens::close(d, src_ne);
    CHECK(lens::remove_tree(d, root, "srccopy").ok(), "clean up");
    lens::close(d, f); lens::close(d, src); lens::close(d, deep);
}

static void ops_suite(const char* realm) {
    Domain* d = new_domain();
    lens::test_unseal();
    fixture(realm);
    char b[64];
    __builtin_snprintf(b, sizeof(b), "%s:/proj", realm);
    Lens root = must_mint(*d, b, ALL);
    ops_data(realm, *d, root);
    ops_dirs(realm, *d, root);
    ops_rename_copy(realm, *d, root);
    lens::close_all(*d);
    CHECK(lens::live_count(*d) == 0, "suite leaked no lens");
    free(d);
}

static void test_cross_realm_and_probe() {
    Domain* d = new_domain();
    lens::test_unseal();
    fixture("ram");
    fixture("disk");
    Lens ram = must_mint(*d, "ram:/proj", ALL);
    Lens disk = must_mint(*d, "disk:/proj", ALL);
    CHECK_L(lens::rename(*d, ram, "readme.txt", disk, "r.txt"), Status::CrossRealm, "rename across realms");
    Lens f = must_open(*d, ram, "readme.txt", Right::Read);
    CHECK(lens::copy(*d, f, disk, "from_ram.txt").ok() && content_is("disk:/proj/from_ram.txt", "hello lens"), "copy ram to disk");
    Lens df = must_open(*d, disk, "src/main.c", Right::Read);
    CHECK(lens::copy(*d, df, ram, "from_disk.c").ok() && content_is("ram:/proj/from_disk.c", "int main;"), "copy disk to ram");
    Lens dsrc = must_open(*d, disk, "src", ALL);
    auto ct = lens::copy_tree(*d, dsrc, ram, "tree");
    if (!ct.ok()) printf("   copy_tree: %s\n", lens::status_name(ct.status()));
    CHECK(ct.ok() && content_is("ram:/proj/tree/deep/leaf.txt", "leaf"), "copy_tree across realms");
    CHECK(lens::remove_tree(*d, ram, "tree").ok() && lens::unlink(*d, ram, "from_disk.c").ok() && lens::unlink(*d, disk, "from_ram.txt").ok(), "cleanup");

    Lens pr = must_mint(*d, "probe:/", ALL);
    Lens ticks = must_open(*d, pr, "ticks", Right::Read);
    CHECK(lens::kind_of(*d, ticks).value() == NodeKind::Probe, "ticks is a probe");
    char a[64], c[64];
    CHECK(read_text(*d, ticks, 0, a, sizeof(a)) && read_text(*d, ticks, 0, c, sizeof(c)) && !same(a, c), "each read from the start renders again");
    CHECK(read_text(*d, ticks, 0, a, sizeof(a)) && read_text(*d, ticks, 3, c, sizeof(c)) && same(c, a + 3), "a continuing read uses the same rendering");
    Lens ver = must_open(*d, pr, "version", Right::Read | Right::Write | Right::Append);
    CHECK(read_text(*d, ver, 0, a, sizeof(a)) && same(a, "name: aphelion\nkind: test\n"), "key: value text");
    CHECK_L(lens::write(*d, ver, 0, txt("x")), Status::ReadOnly, "probes cannot be written");
    CHECK_L(lens::append(*d, ver, txt("x")), Status::ReadOnly, "or appended to");
    CHECK_L(lens::resize(*d, ver, 0), Status::ReadOnly, "or resized");
    CHECK_L(lens::create(*d, pr, "new", Right::Read), Status::ReadOnly, "nothing can be created in a probe realm");
    CHECK_L(lens::unlink(*d, pr, "version"), Status::ReadOnly, "or removed");
    CHECK(lens::copy(*d, ver, ram, "version.txt").ok() && content_is("ram:/proj/version.txt", "name: aphelion\nkind: test\n"), "a probe can be copied to a real file");
    lens::unlink(*d, ram, "version.txt");
    Names v{};
    CHECK(lens::enumerate(*d, pr, &collect_name, &v).ok() && v.count == 2, "probe realm lists its files");

    lens::close_all(*d);
    free(d);
}

static u32 g_wa_calls, g_cf_calls;
static u64 g_wa_bytes;

class CountRealm final : public nebula::Realm {
public:
    const char* name() const override { return "count"; }
    const char* summary() const override { return "disk with counters"; }
    bool writable() const override { return true; }
    bool available() const override { return nebula::disk_realm()->available(); }
    nebula::Node root() override { return nebula::disk_realm()->root(); }
    Status_n find(nebula::Node d, const char* n, nebula::Node* o) override { return nebula::disk_realm()->find(d, n, o); }
    Status_n info(nebula::Node n, nebula::Info* o) override { return nebula::disk_realm()->info(n, o); }
    Status_n list(nebula::Node d, nebula::ChildFn f, void* c) override { return nebula::disk_realm()->list(d, f, c); }
    Status_n read_all(nebula::Node f, void* b, u64 c, u64* s) override { return nebula::disk_realm()->read_all(f, b, c, s); }
    Status_n write_all(nebula::Node f, const void* d, u64 s) override { ++g_wa_calls; g_wa_bytes += s; return nebula::disk_realm()->write_all(f, d, s); }
    Status_n create_file(nebula::Node d, const char* n, nebula::Node* o) override { ++g_cf_calls; return nebula::disk_realm()->create_file(d, n, o); }
    Status_n make_dir(nebula::Node d, const char* n, nebula::Node* o) override { return nebula::disk_realm()->make_dir(d, n, o); }
    Status_n remove(nebula::Node d, const char* n) override { return nebula::disk_realm()->remove(d, n); }
    Status_n rename(nebula::Node a, const char* x, nebula::Node b, const char* y, bool r) override { return nebula::disk_realm()->rename(a, x, b, y, r); }
};
static CountRealm g_count;

static void test_same_traffic_as_nebula() {
    Domain* d = new_domain();
    lens::test_unseal();
    fixture("disk");
    Lens root = must_mint(*d, "count:/proj", ALL);

    static u8 chunk[1000];
    for (u32 i = 0; i < sizeof(chunk); ++i) chunk[i] = static_cast<u8>(i);

    g_wa_calls = 0; g_cf_calls = 0; g_wa_bytes = 0;
    nebula::Handle h;
    CHECK(nebula::open(g_sess, "count:/proj/via_session.bin", nebula::OPEN_READ | nebula::OPEN_WRITE | nebula::OPEN_CREATE, &h) == nebula::Status::Ok, "session open");
    for (int i = 0; i < 200; ++i) nebula::append(g_sess, h, chunk, sizeof(chunk));
    nebula::close(g_sess, h);
    u32 s_wa = g_wa_calls, s_cf = g_cf_calls;
    u64 s_bytes = g_wa_bytes;

    g_wa_calls = 0; g_cf_calls = 0; g_wa_bytes = 0;
    Lens f = lens::create(*d, root, "via_lens.bin", Right::Read | Right::Write | Right::Append).value();
    for (int i = 0; i < 200; ++i) lens::append(*d, f, lens::ConstBytes(chunk, sizeof(chunk)));
    CHECK(g_wa_calls == 0, "no write reaches the realm before sync");
    lens::close(*d, f);
    CHECK(s_wa == 1 && g_wa_calls == 1, "one write_all each for 200 appends");
    CHECK(s_wa == g_wa_calls && s_cf == g_cf_calls && s_bytes == g_wa_bytes, "same write_all, create and byte counts as a Session handle");
    CHECK(g_wa_bytes == 200000, "and the whole file in that one write");

    g_wa_calls = 0;
    nebula::open(g_sess, "count:/proj/via_session.bin", nebula::OPEN_READ | nebula::OPEN_WRITE, &h);
    nebula::write_at(g_sess, h, 10, "abc", 3);
    nebula::write_at(g_sess, h, 500, "def", 3);
    nebula::sync(g_sess, h);
    nebula::sync(g_sess, h);
    nebula::close(g_sess, h);
    u32 s2 = g_wa_calls;
    g_wa_calls = 0;
    f = must_open(*d, root, "via_lens.bin", Right::Read | Right::Write);
    lens::write(*d, f, 10, txt("abc"));
    lens::write(*d, f, 500, txt("def"));
    lens::sync(*d, f);
    lens::sync(*d, f);
    lens::close(*d, f);
    CHECK(s2 == 1 && g_wa_calls == s2, "sync writes once, a clean sync and close write nothing");

    g_wa_calls = 0;
    f = must_open(*d, root, "via_lens.bin", Right::Read);
    char tmp[8];
    lens::read(*d, f, 0, lens::Bytes(reinterpret_cast<u8*>(tmp), 8));
    lens::close(*d, f);
    CHECK(g_wa_calls == 0, "reading writes nothing");

    nebula::Blob x, y;
    CHECK(nebula::read_file(g_sess, "disk:/proj/via_session.bin", &x) == nebula::Status::Ok &&
          nebula::read_file(g_sess, "disk:/proj/via_lens.bin", &y) == nebula::Status::Ok && x.size == y.size && x.size == 200000 &&
          mem::compare(x.data, y.data, x.size) == 0, "identical file contents");
    nebula::release(x);
    nebula::release(y);
    lens::unlink(*d, root, "via_lens.bin");
    lens::unlink(*d, root, "via_session.bin");
    lens::close_all(*d);
    free(d);
}

static void test_epochs() {
    Domain* d = new_domain();
    lens::test_unseal();
    fixture("disk");
    Lens root = must_mint(*d, "disk:/proj", ALL);

    Lens nofreeze = lens::narrow(*d, root, ALL & ~Rights(Right::Freeze)).value();
    CHECK_L(lens::freeze(*d, nofreeze), Status::Denied, "freeze needs the right");
    Lens file = must_open(*d, root, "readme.txt", ALL);
    CHECK_L(lens::freeze(*d, file), Status::NotADirectory, "freeze a file");
    Lens ramroot = must_mint(*d, "ram:/proj", ALL);
    CHECK_L(lens::freeze(*d, ramroot), Status::Unsupported, "only the disk realm can be frozen");
    CHECK_L(lens::freeze(*d, Lens()), Status::BadLens, "freeze through a null lens");
    CHECK(lens::epoch_count(*d) == 0, "refused freezes left no epoch");
    u32 snaps0 = stellar::live_snapshot_count();

    auto fr = lens::freeze(*d, root);
    CHECK(fr.ok() && fr.value().valid(), "freeze");
    Epoch e1 = fr.value();
    CHECK(lens::epoch_count(*d) == 1 && stellar::live_snapshot_count() == snaps0 + 1, "one epoch, one Stellar snapshot");

    Lens w = must_open(*d, root, "readme.txt", Right::Read | Right::Write);
    lens::write(*d, w, 0, txt("HELLO"));
    lens::close(*d, w);
    CHECK(lens::unlink(*d, root, "src/main.c").ok(), "delete a file after the freeze");
    Lens nf = lens::create(*d, root, "later.txt", Right::Write).value();
    lens::write(*d, nf, 0, txt("new"));
    lens::close(*d, nf);
    CHECK(content_is("disk:/proj/readme.txt", "HELLO lens") && !present("disk:/proj/src/main.c"), "live tree moved on");

    auto vr = lens::at(*d, root, e1);
    CHECK(vr.ok(), "at");
    Lens view = vr.value();
    CHECK(lens::kind_of(*d, view).value() == NodeKind::Folder, "a view of a folder is a folder");
    CHECK(lens::read_only_of(*d, view).value(), "views are read-only");
    CHECK(lens::rights_of(*d, view).value() == (Right::Read | Right::Enumerate), "and carry only Read and Enumerate");
    CHECK(lens::epoch_of(*d, view).value() == e1 && !lens::epoch_of(*d, root).value().valid(), "epoch_of");
    Lens old_readme = must_open(*d, view, "readme.txt", Right::Read);
    char buf[64];
    CHECK(read_text(*d, old_readme, 0, buf, sizeof(buf)) && same(buf, "hello lens"), "the view shows the old bytes");
    Lens old_main = must_open(*d, view, "src/main.c", Right::Read);
    CHECK(read_text(*d, old_main, 0, buf, sizeof(buf)) && same(buf, "int main;"), "a deleted file is still there");
    CHECK_L(lens::open_at(*d, view, "later.txt", Right::Read), Status::NotFound, "a file made later is not");
    Names v{};
    CHECK(lens::enumerate(*d, view, &collect_name, &v).ok() && has_name(v, "readme.txt") && has_name(v, "src") && !has_name(v, "later.txt"), "the view lists the old names");
    Names live{};
    lens::enumerate(*d, root, &collect_name, &live);
    CHECK(has_name(live, "later.txt"), "while the live tree lists the new one");

    CHECK_L(lens::open_at(*d, view, ".", Right::Write), Status::Denied, "a view cannot be widened with open_at");
    CHECK(lens::narrow(*d, view, ALL).ok(), "narrow works on a view");
    CHECK(lens::rights_of(*d, lens::narrow(*d, view, ALL).value()).value() == (Right::Read | Right::Enumerate), "but never widens it");
    CHECK_L(lens::write(*d, old_readme, 0, txt("x")), Status::Denied, "write through a view file");
    CHECK_L(lens::create(*d, view, "x", Right::Read), Status::Denied, "create in a view");
    CHECK_L(lens::mkdir(*d, view, "x"), Status::Denied, "mkdir in a view");
    CHECK_L(lens::unlink(*d, view, "readme.txt"), Status::Denied, "unlink in a view");
    CHECK_L(lens::rename(*d, view, "readme.txt", view, "r"), Status::Denied, "rename in a view");
    CHECK_L(lens::freeze(*d, view), Status::Denied, "a view cannot be frozen");
    CHECK_L(lens::delegate(*d, view, *d, Right::Read), Status::Denied, "a view cannot be delegated");
    CHECK(content_is("disk:/proj/readme.txt", "HELLO lens"), "nothing changed in the live tree");
    Lens forced = must_mint(*d, "snap:/", ALL);
    CHECK_L(lens::mkdir(*d, forced, "x"), Status::ReadOnly, "the snap realm itself is read-only");

    Lens src = must_open(*d, root, "src", ALL);
    Lens src_view = lens::at(*d, src, e1).value();
    CHECK(lens::kind_of(*d, src_view).value() == NodeKind::Folder, "view of a lens below the frozen folder");
    Lens old_leaf = must_open(*d, src_view, "deep/leaf.txt", Right::Read);
    CHECK(read_text(*d, old_leaf, 0, buf, sizeof(buf)) && same(buf, "leaf"), "and it reads");
    Lens parent_of_frozen = must_mint(*d, "disk:/", ALL);
    CHECK_L(lens::at(*d, parent_of_frozen, e1), Status::NotFound, "a lens above the frozen folder has no past in this epoch");
    Lens later_file = must_open(*d, root, "later.txt", Right::Read);
    CHECK_L(lens::at(*d, later_file, e1), Status::NotFound, "a node that did not exist at the epoch");

    CHECK_L(lens::at(*d, root, Epoch()), Status::NoSuchEpoch, "no epoch");
    CHECK_L(lens::at(*d, root, Epoch(999999)), Status::NoSuchEpoch, "unknown epoch");
    stellar::Status why;
    u64 foreign = stellar::snapshot(&why);
    CHECK(foreign != stellar::INVALID_STAR, "take a snapshot outside the lens layer");
    CHECK_L(lens::at(*d, root, Epoch(foreign)), Status::NoSuchEpoch, "a snapshot this domain did not freeze is not its epoch");
    CHECK_L(lens::release_epoch(*d, Epoch(foreign)), Status::NoSuchEpoch, "and cannot be released through it");
    CHECK(stellar::snapshot_scope(foreign) != stellar::INVALID_STAR, "the foreign snapshot is untouched");
    Domain* other = new_domain();
    Lens oroot = must_mint(*other, "disk:/proj", ALL);
    CHECK_L(lens::at(*other, oroot, e1), Status::NoSuchEpoch, "another domain cannot use this epoch");
    CHECK_L(lens::release_epoch(*other, e1), Status::NoSuchEpoch, "or release it");
    lens::close_all(*other);
    free(other);
    stellar::delete_snapshot(foreign);

    Lens blind = lens::narrow(*d, root, Right::Write | Right::Freeze).value();
    CHECK_L(lens::at(*d, blind, e1), Status::Denied, "at needs Read or Enumerate");
    Lens ramview = lens::narrow(*d, ramroot, ALL).value();
    CHECK_L(lens::at(*d, ramview, e1), Status::Unsupported, "at on a non-disk lens");

    Lens live_dir = root;
    auto r1 = lens::restore(*d, old_readme, live_dir, "readme.txt");
    CHECK(r1.ok() && r1.value() == 1 && content_is("disk:/proj/readme.txt", "hello lens"), "restore a file over its newer self");
    auto r2 = lens::restore(*d, old_main, live_dir, "src/main.c");
    CHECK(r2.ok() && content_is("disk:/proj/src/main.c", "int main;"), "restore a deleted file");
    CHECK(lens::unlink(*d, root, "src/deep/leaf.txt").ok(), "delete another file");
    auto r3 = lens::restore(*d, src_view, live_dir, "src_back");
    CHECK(r3.ok() && r3.value() == 2 && content_is("disk:/proj/src_back/deep/leaf.txt", "leaf"), "restore a folder under a new name");
    auto r4 = lens::restore(*d, src_view, src, ".");
    CHECK(r4.status() == Status::InvalidPath, "restore needs a name");
    auto r5 = lens::restore(*d, view, live_dir, "whole");
    CHECK(r5.ok() && r5.value() == 4 && content_is("disk:/proj/whole/readme.txt", "hello lens") && !present("disk:/proj/whole/later.txt"), "restore the whole epoch beside the live tree");
    CHECK_L(lens::restore(*d, root, live_dir, "x"), Status::Unsupported, "restore needs a view as its source");
    Lens ro_live = lens::narrow(*d, root, ALL, true).value();
    CHECK_L(lens::restore(*d, old_readme, ro_live, "r"), Status::ReadOnly, "restore into a read-only lens");
    Lens nocreate = lens::narrow(*d, root, Right::Read | Right::Enumerate).value();
    CHECK_L(lens::restore(*d, old_readme, nocreate, "fresh"), Status::Denied, "restore needs Create or Write on the live side");
    CHECK(!present("disk:/proj/fresh"), "denied restore wrote nothing");
    CHECK(lens::remove_tree(*d, root, "src_back").ok() && lens::remove_tree(*d, root, "whole").ok(), "clean up restores");

    Lens w2 = must_open(*d, root, "readme.txt", Right::Read | Right::Write);
    lens::write(*d, w2, 0, txt("CHANGED AGAIN"));
    lens::close(*d, w2);
    Lens old_again = must_open(*d, view, "readme.txt", Right::Read);
    CHECK(read_text(*d, old_again, 0, buf, sizeof(buf)) && same(buf, "hello lens"), "the view is frozen");

    CHECK(lens::release_epoch(*d, e1).ok() && lens::epoch_count(*d) == 0, "release");
    CHECK(stellar::live_snapshot_count() == snaps0, "the Stellar snapshot is gone");
    CHECK_L(lens::at(*d, root, e1), Status::NoSuchEpoch, "released epochs are unknown");
    CHECK_L(lens::release_epoch(*d, e1), Status::NoSuchEpoch, "release twice");
    CHECK_L(lens::stat(*d, view), Status::NotFound, "and does, with NotFound");

    lens::close(*d, root);
    CHECK_L(lens::freeze(*d, root), Status::StaleLens, "freeze through a closed lens");

    lens::close_all(*d);
    free(d);
}

static void test_epoch_limits() {
    Domain* d = new_domain();
    lens::test_unseal();
    fixture("disk");
    Lens root = must_mint(*d, "disk:/proj", ALL);
    u32 base = stellar::live_snapshot_count();

    Epoch es[lens::MAX_EPOCHS];
    u32 n = 0;
    for (; n < lens::MAX_EPOCHS; ++n) {
        auto f = lens::freeze(*d, root);
        if (!f.ok()) break;
        es[n] = f.value();
    }
    CHECK(n == lens::MAX_EPOCHS, "the domain can hold its full epoch table");
    bool ascending = true;
    for (u32 i = 1; i < n; ++i) if (!(es[i - 1] < es[i])) ascending = false;
    CHECK(ascending, "epochs are distinct and ordered");
    u32 snaps_full = stellar::live_snapshot_count();
    CHECK_L(lens::freeze(*d, root), Status::TooManyEpochs, "one past the table");
    CHECK(stellar::live_snapshot_count() == snaps_full, "the refused freeze made no snapshot");
    CHECK(lens::release_epoch(*d, es[7]).ok() && lens::freeze(*d, root).ok(), "release one and freeze again");
    lens::close_all(*d);
    for (u32 i = 0; i < n; ++i) if (i != 7) lens::release_epoch(*d, es[i]);

    for (u32 i = 0; i < lens::MAX_EPOCHS; ++i) if (d->epochs[i]) lens::release_epoch(*d, Epoch(d->epochs[i]));
    CHECK(lens::epoch_count(*d) == 0 && stellar::live_snapshot_count() == base, "everything released");

    root = must_mint(*d, "disk:/proj", ALL);
    u32 made = 0;
    u64 ids[stellar::SNAPSHOT_MAX];
    stellar::Status why;

    u64 proj = stellar::resolve("/proj");
    while (stellar::live_snapshot_count() < stellar::SNAPSHOT_MAX && made < stellar::SNAPSHOT_MAX) {
        u64 id = stellar::snapshot_subtree(proj, &why);
        if (id == stellar::INVALID_STAR) break;
        ids[made++] = id;
    }
    CHECK(stellar::live_snapshot_count() == stellar::SNAPSHOT_MAX, "Stellar is at its snapshot ceiling");
    CHECK_L(lens::freeze(*d, root), Status::TooManyEpochs, "freeze at the ceiling");
    CHECK(lens::epoch_count(*d) == 0, "and no epoch was recorded for it");
    for (u32 i = 0; i < made; ++i) stellar::delete_snapshot(ids[i]);
    CHECK(stellar::live_snapshot_count() == base, "back to where we were");
    CHECK(lens::freeze(*d, root).ok(), "freeze works again once there is room");
    for (u32 i = 0; i < lens::MAX_EPOCHS; ++i) if (d->epochs[i]) lens::release_epoch(*d, Epoch(d->epochs[i]));

    nebula::make_path(g_sess, "disk:/small");
    nebula::make_path(g_sess, "disk:/large");
    put("disk:/small/a", "x");
    for (int i = 0; i < 400; ++i) {
        char p[64];
        __builtin_snprintf(p, sizeof(p), "disk:/large/d%d", i % 20);
        nebula::make_path(g_sess, p);
        __builtin_snprintf(p, sizeof(p), "disk:/large/d%d/f%d", i % 20, i);
        put(p, "y");
    }
    Lens small = must_mint(*d, "disk:/small", ALL);
    Lens large = must_mint(*d, "disk:/large", ALL);
    stellar::IoStats s0 = stellar::io_stats();
    Epoch es_small = lens::freeze(*d, small).value();
    stellar::IoStats s1 = stellar::io_stats();
    Epoch es_large = lens::freeze(*d, large).value();
    stellar::IoStats s2 = stellar::io_stats();
    CHECK(es_small.valid() && es_large.valid(), "both froze");
    CHECK((s2.writes - s1.writes) > (s1.writes - s0.writes), "freezing a bigger subtree writes more");
    lens::release_epoch(*d, es_small);
    lens::release_epoch(*d, es_large);
    nebula::remove_tree(g_sess, "disk:/small", nullptr);
    nebula::remove_tree(g_sess, "disk:/large", nullptr);
    CHECK(stellar::live_snapshot_count() == base, "no snapshots left behind");

    lens::close_all(*d);
    free(d);
}

static Lens reset_proj(Domain& d, Lens old) {
    if (!old.is_null()) (void)lens::close(d, old);
    fixture("disk");
    return must_mint(d, "disk:/proj", ALL);
}

static int batch_signature() {
    bool before = content_is("disk:/proj/readme.txt", "hello lens") && !present("disk:/proj/archive");
    bool after = content_is("disk:/proj/readme.txt", "v2") && content_is("disk:/proj/archive/readme.old", "hello lens") &&
                 content_is("disk:/proj/archive/log", "moved");
    return before ? 1 : (after ? 2 : 0);
}

static lens::Done run_batch(Domain& d, Lens root) {
    lens::Batch b;
    b.note(lens::mkdir(d, root, "archive"));
    b.note(lens::rename(d, root, "readme.txt", root, "archive/readme.old"));
    b.note(lens::put(d, root, "readme.txt", txt("v2")));
    b.note(lens::put(d, root, "archive/log", txt("moved")));
    return b.commit();
}

static bool volume_clean() {
    stellar::CheckReport rep{};
    return stellar::check(&rep, true) && rep.ok();
}

static void test_batch_basics() {
    Domain* d = new_domain();
    lens::test_unseal();
    fixture("disk");
    Lens root = must_mint(*d, "disk:/proj", ALL);
    CHECK(batch_signature() == 1, "starting state");

    CHECK(run_batch(*d, root).ok(), "batch commits");
    CHECK(batch_signature() == 2, "all three changes are visible");
    CHECK(stellar::mount(), "remount");
    CHECK(batch_signature() == 2, "and durable");
    CHECK(volume_clean(), "volume checks clean");

    root = reset_proj(*d, root);
    {
        lens::Batch b;
        CHECK(b.note(lens::mkdir(*d, root, "archive")).ok(), "mkdir inside the batch");
        CHECK(b.note(lens::rename(*d, root, "readme.txt", root, "archive/readme.old")).ok(), "rename inside the batch");
        CHECK(present("disk:/proj/archive/readme.old"), "inside the batch the changes are visible");
        auto bad = b.note(lens::unlink(*d, root, "does-not-exist"));
        CHECK(bad.status() == Status::NotFound && b.failed() && b.first_error() == Status::NotFound, "a failed op is recorded");
        b.note(lens::put(*d, root, "later", txt("after the failure")));
        auto r = b.commit();
        CHECK(r.status() == Status::NotFound, "commit reports the first failure");
        CHECK(!b.open(), "and ends the batch");
    }
    CHECK(batch_signature() == 1 && !present("disk:/proj/later") && !present("disk:/proj/archive"), "everything was undone");
    CHECK(stellar::mount() && batch_signature() == 1 && volume_clean(), "also after a remount");
    CHECK(lens::unlink(*d, root, "./a:b").ok(), "Stellar is usable afterwards");
    nebula::write_file(g_sess, "disk:/proj/a:b", "colon name", 10);
    CHECK(!stellar::end_batch(), "no batch is left open");

    root = reset_proj(*d, root);
    {
        lens::Batch b;
        b.note(lens::mkdir(*d, root, "archive"));
        b.note(lens::put(*d, root, "archive/x", txt("x")));
        CHECK(present("disk:/proj/archive/x"), "work is visible inside the batch");
    }
    CHECK(!present("disk:/proj/archive") && batch_signature() == 1, "destructor without commit rolls back");

    {
        lens::Batch b;
        b.note(lens::put(*d, root, "tmp", txt("t")));
        b.abort();
        CHECK(!b.open(), "abort ends the batch");
        CHECK(b.commit().status() == Status::BatchFailed, "commit after abort");
        b.abort();
    }
    CHECK(!present("disk:/proj/tmp"), "abort undid the put");

    { lens::Batch b; CHECK(b.commit().ok(), "empty batch"); }

    {
        lens::Batch b;
        CHECK(b.commit().ok() && b.commit().status() == Status::BatchFailed, "commit twice");
    }

    {
        lens::Batch b;
        auto c = b.note(lens::create(*d, root, "noted", Right::Read | Right::Write));
        CHECK(c.ok() && !c.value().is_null(), "note keeps the result's value");
        auto e = b.note(lens::create(*d, root, "noted", Right::Read));
        CHECK(e.status() == Status::Exists && b.failed(), "and the failure");
        lens::close(*d, c.value());
        b.abort();
    }
    CHECK(!present("disk:/proj/noted"), "the batch took the created file back");

    {
        Lens ro = lens::narrow(*d, root, ALL, true).value();
        lens::Batch b;
        b.note(lens::put(*d, root, "ok-file", txt("fine")));
        b.note(lens::put(*d, ro, "no-file", txt("nope")));
        CHECK(b.commit().status() == Status::ReadOnly, "read-only failure poisons the batch");
        lens::close(*d, ro);
    }
    CHECK(!present("disk:/proj/ok-file") && !present("disk:/proj/no-file"), "neither file exists");

    root = reset_proj(*d, root);
    {
        lens::Batch b;
        Lens f = lens::create(*d, root, "buffered", Right::Read | Right::Write).value();
        b.note(lens::write(*d, f, 0, txt("data")));
        b.note(lens::sync(*d, f));
        lens::close(*d, f);
        b.abort();
    }
    CHECK(!present("disk:/proj/buffered"), "a synced write inside an aborted batch is undone too");

    Lens ramroot = must_mint(*d, "ram:/proj", ALL);
    CHECK(lens::Batch::covers(*d, root) && lens::Batch::covers(*d, ramroot), "covers() reports disk and ram");
    CHECK(!lens::Batch::covers(*d, Lens()), "covers() on a null lens");
    {
        lens::Batch b;
        b.note(lens::put(*d, ramroot, "ram-note", txt("stays")));
        b.abort();
    }
    CHECK(!present("ram:/proj/ram-note"), "ram operations are undone with the batch");

    lens::close_all(*d);
    free(d);
}

static void test_batch_failure_injection() {
    Domain* d = new_domain();
    lens::test_unseal();
    fixture("disk");
    Lens root = must_mint(*d, "disk:/proj", ALL);

    g_wcount = 0;
    g_last_sb_w = 0;
    CHECK(run_batch(*d, root).ok(), "clean run");
    u64 total = g_last_sb_w ? g_last_sb_w : g_wcount;
    CHECK(total > 4, "the batch writes several sectors");
    printf("   batch is %lu sector writes up to its commit point\n", total);

    u32 rolled_back = 0, committed_anyway = 0, mixed = 0, unreported = 0, dirty = 0, remount_changed = 0;
    for (u64 k = 0; k < total; ++k) {
        root = reset_proj(*d, root);
        g_wcount = 0;
        g_fail_after = k;
        lens::Done r = run_batch(*d, root);
        g_fail_after = ~0ull;
        if (r.ok()) ++unreported;
        int s = batch_signature();
        if (s == 0) ++mixed;
        else if (s == 1) ++rolled_back;
        else ++committed_anyway;
        if (!volume_clean()) ++dirty;
        stellar::mount();
        if (batch_signature() != s) ++remount_changed;
        if (!volume_clean()) ++dirty;
    }
    CHECK(unreported == 0, "a failed write always fails the commit");
    CHECK(mixed == 0, "never a half-applied batch");
    CHECK(dirty == 0, "the volume checks clean after every injected failure");
    CHECK(remount_changed == 0, "what the batch left in memory is what a reboot sees");
    CHECK(rolled_back > 0, "most injected failures roll the batch back");
    printf("   %lu injected failures: %u rolled back, %u durable despite the error, none mixed\n", total, rolled_back, committed_anyway);

    root = reset_proj(*d, root);
    CHECK(run_batch(*d, root).ok() && batch_signature() == 2, "a healthy disk batches again");
    lens::close_all(*d);
    free(d);
}

static void test_batch_group_commit() {
    Domain* d = new_domain();
    lens::test_unseal();
    fixture("disk");
    Lens root = must_mint(*d, "disk:/proj", ALL);
    char name[16];

    stellar::IoStats a0 = stellar::io_stats();
    for (int i = 0; i < 50; ++i) {
        __builtin_snprintf(name, sizeof(name), "solo%d", i);
        lens::put(*d, root, name, txt("x"));
    }
    stellar::IoStats a1 = stellar::io_stats();
    {
        lens::Batch b;
        for (int i = 0; i < 50; ++i) {
            __builtin_snprintf(name, sizeof(name), "grp%d", i);
            b.note(lens::put(*d, root, name, txt("x")));
        }
        CHECK(b.commit().ok(), "50 puts in one batch");
    }
    stellar::IoStats a2 = stellar::io_stats();
    printf("   50 puts: %lu sector writes one by one, %lu in one batch\n", a1.writes - a0.writes, a2.writes - a1.writes);
    CHECK((a2.writes - a1.writes) < (a1.writes - a0.writes), "one commit costs fewer writes than fifty");
    CHECK(content_is("disk:/proj/grp49", "x") && content_is("disk:/proj/solo49", "x"), "both ways stored the data");
    lens::close_all(*d);
    free(d);
}

struct LockProbe {
    Domain* d;
    Lens l;
    volatile int started;
    volatile int done;
};
static void* probe_thread(void* p) {
    LockProbe* lp = static_cast<LockProbe*>(p);
    lp->started = 1;
    lens::stat(*lp->d, lp->l);
    lp->done = 1;
    return nullptr;
}

static int ram_signature() {
    bool before = content_is("ram:/proj/readme.txt", "hello lens") && !present("ram:/proj/archive");
    bool after = content_is("ram:/proj/readme.txt", "v2") && content_is("ram:/proj/archive/readme.old", "hello lens") &&
                 content_is("ram:/proj/archive/log", "moved");
    return before ? 1 : (after ? 2 : 0);
}

static Lens reset_ram(Domain& d, Lens old) {
    if (!old.is_null()) (void)lens::close(d, old);
    fixture("ram");
    return must_mint(d, "ram:/proj", ALL);
}

static bool ram_tree_untouched() {
    return content_is("ram:/proj/readme.txt", "hello lens") && content_is("ram:/proj/src/main.c", "int main;") &&
           content_is("ram:/proj/src/deep/leaf.txt", "leaf") && content_is("ram:/proj/a:b", "colon name") &&
           !present("ram:/proj/archive");
}

static void test_batch_ram() {
    Domain* d = new_domain();
    lens::test_unseal();
    fixture("ram");
    Lens root = must_mint(*d, "ram:/proj", ALL);
    u64 free_before = nebula::ram_realm()->space_free();

    CHECK(lens::Batch::covers(*d, root), "a ram lens is covered by batches");
    Lens sys_root = must_mint(*d, "sys:/", Right::Read | Right::Enumerate);
    CHECK(lens::Batch::covers(*d, sys_root), "a read-only realm has nothing to undo");

    {
        lens::Batch b;
        b.note(lens::mkdir(*d, root, "archive"));
        b.note(lens::rename(*d, root, "readme.txt", root, "archive/readme.old"));
        b.note(lens::put(*d, root, "readme.txt", txt("v2")));
        b.note(lens::put(*d, root, "archive/log", txt("moved")));
        CHECK(b.commit().ok(), "ram batch commits");
    }
    CHECK(ram_signature() == 2, "all three changes are visible");

    root = reset_ram(*d, root);
    CHECK(ram_signature() == 1, "reset");
    {
        lens::Batch b;
        b.note(lens::mkdir(*d, root, "archive"));
        b.note(lens::rename(*d, root, "readme.txt", root, "archive/readme.old"));
        b.note(lens::put(*d, root, "readme.txt", txt("v2")));
        b.note(lens::unlink(*d, root, "does-not-exist"));
        lens::Done r = b.commit();
        CHECK(!r.ok() && r.status() == Status::NotFound, "the first failure is the batch's result");
    }
    CHECK(ram_tree_untouched(), "a failed ram batch leaves the tree exactly as it was");

    {
        lens::Batch b;
        b.note(lens::put(*d, root, "readme.txt", txt("x")));
        b.note(lens::put(*d, root, "src/main.c", txt("a much longer replacement than before, past the old capacity")));
        b.note(lens::put(*d, root, "fresh.txt", txt("new")));
        b.note(lens::unlink(*d, root, "a:b"));
        b.note(lens::remove_tree(*d, root, "src/deep"));
        b.note(lens::mkdir(*d, root, "made/deeper", true));
        b.abort();
    }
    CHECK(ram_tree_untouched() && !present("ram:/proj/fresh.txt") && !present("ram:/proj/made"), "abort undoes overwrite, shrink, unlink, remove_tree and mkdir");

    {
        lens::Batch b;
        b.note(lens::put(*d, root, "readme.txt", txt("")));
        b.note(lens::put(*d, root, "readme.txt", txt("second write in the same batch")));
        b.note(lens::put(*d, root, "readme.txt", txt("3")));
    }
    CHECK(ram_tree_untouched(), "leaving scope without commit rolls back repeated writes to one file");

    {
        lens::Batch b;
        b.note(lens::put(*d, root, "readme.txt", txt("")));
        b.note(lens::put(*d, root, "readme.txt", txt("kept after several writes")));
        CHECK(b.commit().ok(), "commit with repeated writes");
    }
    CHECK(content_is("ram:/proj/readme.txt", "kept after several writes"), "the last write wins");

    root = reset_ram(*d, root);
    CHECK(nebula::ram_realm()->space_free() <= free_before + 4096 && nebula::ram_realm()->space_free() + 4096 >= free_before, "byte accounting survives commits and rollbacks");

    {
        lens::Batch b;
        b.note(lens::remove_tree(*d, root, "src"));
        b.note(lens::rename(*d, root, "readme.txt", root, "src"));
        CHECK(b.commit().ok(), "remove then reuse a name in one batch");
    }
    CHECK(content_is("ram:/proj/src", "hello lens") && !present("ram:/proj/readme.txt"), "the rename landed");
    root = reset_ram(*d, root);
    {
        lens::Batch b;
        b.note(lens::remove_tree(*d, root, "src"));
        b.note(lens::rename(*d, root, "readme.txt", root, "src"));
        b.fail(Status::Io);
        CHECK(!b.commit().ok(), "a failure after reuse fails the batch");
    }
    CHECK(ram_tree_untouched(), "and the removed tree comes back whole");

    lens::close_all(*d);
    free(d);
}

static void test_batch_spans_realms() {
    Domain* d = new_domain();
    lens::test_unseal();
    fixture("disk");
    fixture("ram");
    Lens disk = must_mint(*d, "disk:/proj", ALL);
    Lens ram = must_mint(*d, "ram:/proj", ALL);

    {
        lens::Batch b;
        b.note(lens::put(*d, ram, "both.txt", txt("ram side")));
        b.note(lens::put(*d, disk, "both.txt", txt("disk side")));
        CHECK(b.commit().ok(), "one batch commits on both realms");
    }
    CHECK(content_is("ram:/proj/both.txt", "ram side") && content_is("disk:/proj/both.txt", "disk side"), "both sides landed");

    {
        lens::Batch b;
        b.note(lens::put(*d, ram, "only.txt", txt("ram")));
        b.note(lens::put(*d, disk, "only.txt", txt("disk")));
        b.note(lens::unlink(*d, disk, "missing"));
        CHECK(!b.commit().ok(), "a failure on one realm fails the batch");
    }
    CHECK(!present("ram:/proj/only.txt") && !present("disk:/proj/only.txt"), "neither realm keeps its half");

    {
        lens::Batch b;
        b.note(lens::put(*d, disk, "d.txt", txt("disk first")));
        b.note(lens::put(*d, ram, "r.txt", txt("ram second")));
        b.note(lens::unlink(*d, ram, "missing"));
        b.abort();
    }
    CHECK(!present("ram:/proj/r.txt") && !present("disk:/proj/d.txt"), "abort covers both realms in either order");
    CHECK(volume_clean(), "volume checks clean");

    lens::close_all(*d);
    free(d);
}

static void test_batch_holds_the_lock() {
    Domain* d = new_domain();
    lens::test_unseal();
    fixture("disk");
    Lens root = must_mint(*d, "disk:/proj", ALL);
    Lens other = lens::narrow(*d, root, Right::Read).value();
    LockProbe lp{d, other, 0, 0};
    pthread_t th;
    {
        lens::Batch b;
        b.note(lens::put(*d, root, "held", txt("x")));
        pthread_create(&th, nullptr, &probe_thread, &lp);
        for (int i = 0; i < 200 && !lp.started; ++i) usleep(1000);
        usleep(150000);
        CHECK(lp.started == 1 && lp.done == 0, "another task's disk call waits while a batch is open");
        CHECK(b.commit().ok(), "commit");
    }
    pthread_join(th, nullptr);
    CHECK(lp.done == 1, "and proceeds once the batch ends");
    lens::unlink(*d, root, "held");
    lens::close_all(*d);
    free(d);
}

struct KV { char key[64]; char val[200]; };

static int parse_kv(const char* text, KV* out, int cap) {
    int n = 0;
    const char* p = text;
    while (*p) {
        const char* eol = p;
        while (*eol && *eol != '\n') ++eol;
        if (!*eol) return -1;
        const char* colon = p;
        while (colon < eol && *colon != ':' && *colon != ' ') ++colon;
        if (colon == p || colon >= eol || *colon != ':' || colon[1] != ' ' || eol == colon + 2) return -1;
        if (n >= cap) return -1;
        u32 kl = static_cast<u32>(colon - p), vl = static_cast<u32>(eol - (colon + 2));
        if (kl >= sizeof(out[n].key) || vl >= sizeof(out[n].val)) return -1;
        mem::copy(out[n].key, p, kl); out[n].key[kl] = 0;
        mem::copy(out[n].val, colon + 2, vl); out[n].val[vl] = 0;
        ++n;
        p = eol + 1;
    }
    return n;
}

static const char* kv_find(const KV* v, int n, const char* key) {
    for (int i = 0; i < n; ++i) if (same(v[i].key, key)) return v[i].val;
    return nullptr;
}

static void test_sys_probes() {
    Domain* d = new_domain();
    lens::test_unseal();
    g_uptime_fixed = 3723004;
    Lens sys = must_mint(*d, "sys:/", Right::Read | Right::Enumerate);
    Names names{};
    CHECK(lens::enumerate(*d, sys, &collect_name, &names).ok() && names.count == 8, "sys has eight files");

    static KV kv[64];
    char buf[8192];
    const char* keyed[] = {"version", "uptime", "memory", "cpus", "tasks", "realms", "storage", "log"};
    for (const char* f : keyed) {
        Lens p = must_open(*d, sys, f, Right::Read);
        CHECK(lens::kind_of(*d, p).value() == NodeKind::Probe, "every sys file is a probe");
        CHECK(read_text(*d, p, 0, buf, sizeof(buf)), "sys file reads");
        int n = parse_kv(buf, kv, 64);
        if (n < 1) printf("   %s is not key: value text:\n%s\n", f, buf);
        CHECK(n >= 1, "every line of a sys file is 'key: value'");
        CHECK_L(lens::write(*d, p, 0, txt("x")), Status::Denied, "no Write right, no write");
        lens::close(*d, p);
    }

    auto load = [&](const char* f, int* n) {
        Lens p = must_open(*d, sys, f, Right::Read);
        read_text(*d, p, 0, buf, sizeof(buf));
        *n = parse_kv(buf, kv, 64);
        lens::close(*d, p);
    };
    int n;
    load("version", &n);
    CHECK(n == 3 && cstr::find(kv_find(kv, n, "kernel"), "Aphelion x86_64") != nullptr, "version names the kernel");
    CHECK(same(kv_find(kv, n, "suns"), "1") && same(kv_find(kv, n, "hypervisor"), "yes"), "version fields");
    load("uptime", &n);
    CHECK(same(kv_find(kv, n, "uptime_ms"), "3723004") && same(kv_find(kv, n, "uptime"), "1h 2m 3s"), "uptime fields");
    load("memory", &n);
    CHECK(same(kv_find(kv, n, "universe_free"), "102400 KiB") && same(kv_find(kv, n, "heap_objects"), "12"), "memory fields");
    load("cpus", &n);
    CHECK(same(kv_find(kv, n, "sun.0"), "apic=0 state=running queued=2 task=helm"), "cpu line");
    CHECK(same(kv_find(kv, n, "sun.1"), "apic=1 state=idle queued=0 task=idle-1") && same(kv_find(kv, n, "preemptions"), "9"), "second cpu and counters");
    load("tasks", &n);
    CHECK(n == 2 && same(kv_find(kv, n, "task.1"), "state=running ring=0 core=0 switches=40 name=helm"), "task line");
    CHECK(same(kv_find(kv, n, "task.12"), "state=sleeping ring=3 core=1 switches=5 name=net rx"), "task names may contain spaces");
    load("realms", &n);
    CHECK(kv_find(kv, n, "disk") && cstr::find(kv_find(kv, n, "disk"), "state=available access=writable summary=") != nullptr, "realm line");
    CHECK(kv_find(kv, n, "snap") && cstr::find(kv_find(kv, n, "snap"), "access=read-only") != nullptr, "read-only realm");
    CHECK(kv_find(kv, n, "sys") != nullptr && kv_find(kv, n, "probe") != nullptr, "every registered realm is listed");
    load("storage", &n);
    CHECK(kv_find(kv, n, "device") != nullptr && kv_find(kv, n, "disk_free") != nullptr && kv_find(kv, n, "snapshots") != nullptr, "storage fields");

    load("log", &n);
    CHECK(n == 2 && same(kv_find(kv, n, "log.0"), "boot: hello") && same(kv_find(kv, n, "log.12"), "stellar: mounted: ok"), "the log is one 'log.SEQ: text' line per kernel log line");

    Lens up = must_open(*d, sys, "uptime", Right::Read);
    char a[64], b[64];
    CHECK(read_text(*d, up, 0, a, sizeof(a)) && read_text(*d, up, 4, b, sizeof(b)) && same(b, a + 4), "continuing read agrees with the first");
    g_uptime_fixed = 0;
    lens::close_all(*d);
    free(d);
}

class FakeConsole final : public lens::Stream {
public:
    char in[256]; u32 in_len = 0, in_pos = 0;
    char out[256]; u32 out_len = 0;
    u32 refs = 0, releases = 0;
    const char* name() const override { return "console"; }
    lens::Result<usize> read(lens::Bytes dst) override {
        if (in_pos == in_len) return lens::Result<usize>::fail(Status::NotReady);
        usize n = dst.len < in_len - in_pos ? dst.len : in_len - in_pos;
        mem::copy(dst.ptr, in + in_pos, n);
        in_pos += n;
        return lens::Result<usize>::success(n);
    }
    lens::Result<usize> write(lens::ConstBytes src) override {
        usize n = src.len < sizeof(out) - out_len ? src.len : sizeof(out) - out_len;
        mem::copy(out + out_len, src.ptr, n);
        out_len += n;
        return lens::Result<usize>::success(n);
    }
    lens::Readiness ready() override {
        lens::Readiness r = lens::Ready::Writable;
        if (in_pos < in_len) r = r | lens::Ready::Readable;
        return r;
    }
    void retain() override { ++refs; }
    void release() override { ++releases; --refs; }
    void type(const char* s) { in_len = cstr::length(s); mem::copy(in, s, in_len); in_pos = 0; }
};

static bool read_exact(Domain& d, Lens l, const char* want) {
    char b[512];
    auto r = lens::read(d, l, 0, lens::Bytes(reinterpret_cast<u8*>(b), sizeof(b) - 1));
    if (!r.ok()) return false;
    b[r.value()] = 0;
    return same(b, want);
}

static void test_pipes() {
    Domain* d = new_domain();
    lens::test_unseal();
    Lens rd, wr;
    CHECK(lens::make_pipe(*d, 100, &rd, &wr).ok(), "make a pipe");
    CHECK(lens::kind_of(*d, rd).value() == NodeKind::Stream && lens::kind_of(*d, wr).value() == NodeKind::Stream, "both ends are streams");
    CHECK(lens::rights_of(*d, rd).value() == (Right::Read | Right::Delegate), "the read end reads, and can be handed on");
    CHECK(lens::rights_of(*d, wr).value() == (Right::Write | Right::Append | Right::Delegate), "the write end writes, and can be handed on");
    CHECK(lens::live_count(*d) == 2, "two lenses");

    char b[64];
    CHECK_L(lens::read(*d, rd, 0, lens::Bytes(reinterpret_cast<u8*>(b), 8)), Status::NotReady, "reading an empty pipe");
    CHECK_L(lens::read(*d, wr, 0, lens::Bytes(reinterpret_cast<u8*>(b), 8)), Status::Denied, "reading the write end");
    CHECK_L(lens::write(*d, rd, 0, txt("x")), Status::Denied, "writing the read end");
    CHECK(lens::write(*d, wr, 999, txt("abc")).value() == 3, "write ignores the offset");
    CHECK(lens::append(*d, wr, txt("def")).ok(), "append is a write");
    CHECK(read_exact(*d, rd, "abcdef"), "bytes come out in order, offset ignored");
    CHECK_L(lens::read(*d, rd, 0, lens::Bytes(reinterpret_cast<u8*>(b), 8)), Status::NotReady, "and it is empty again");
    CHECK(lens::write(*d, wr, 0, lens::ConstBytes()).value() == 0, "an empty write is fine");

    u8 big[300];
    for (u32 i = 0; i < sizeof(big); ++i) big[i] = static_cast<u8>(i);
    auto w1 = lens::write(*d, wr, 0, lens::ConstBytes(big, sizeof(big)));
    CHECK(w1.ok() && w1.value() == 128, "100 rounds up to 128 and a larger write is cut to it");
    CHECK_L(lens::write(*d, wr, 0, txt("x")), Status::NotReady, "a full pipe takes nothing");
    u8 got[300];
    auto r1 = lens::read(*d, rd, 0, lens::Bytes(got, 50));
    CHECK(r1.ok() && r1.value() == 50 && mem::compare(got, big, 50) == 0, "a short read takes the front");
    auto w2 = lens::write(*d, wr, 0, lens::ConstBytes(big + 128, 100));
    CHECK(w2.ok() && w2.value() == 50, "freed room can be refilled");

    bool ordered = true;
    u8 next_out = 50, next_in = 178;
    u32 total = 0;
    for (int round = 0; round < 5000 && ordered; ++round) {
        u8 tmp[37];
        auto r = lens::read(*d, rd, 0, lens::Bytes(tmp, 1 + round % 37));
        if (r.ok()) for (usize i = 0; i < r.value(); ++i) { if (tmp[i] != next_out++) ordered = false; ++total; }
        u8 src[29];
        for (u32 i = 0; i < sizeof(src); ++i) src[i] = static_cast<u8>(next_in + i);
        auto w = lens::write(*d, wr, 0, lens::ConstBytes(src, 1 + round % 29));
        if (w.ok()) next_in = static_cast<u8>(next_in + w.value());
    }
    CHECK(ordered && total > 20000, "tens of thousands of bytes through a 128 byte ring arrive in order");
    while (lens::read(*d, rd, 0, lens::Bytes(got, sizeof(got))).ok() && false) {}
    lens::close_all(*d);

    CHECK(lens::make_pipe(*d, 1, &rd, &wr).ok(), "tiny pipe");
    CHECK(lens::write(*d, wr, 0, lens::ConstBytes(big, 200)).value() == 64, "smallest pipe holds 64 bytes");
    lens::close_all(*d);
    CHECK(lens::make_pipe(*d, 100u << 20, &rd, &wr).ok(), "huge request");
    static u8 chunk[1 << 20];
    CHECK(lens::write(*d, wr, 0, lens::ConstBytes(chunk, sizeof(chunk))).value() == (1u << 20), "is capped at 1 MiB");
    lens::close_all(*d);

    CHECK(lens::make_pipe(*d, 64, &rd, &wr).ok(), "pipe");
    CHECK_L(lens::size(*d, rd), Status::Unsupported, "size");
    CHECK_L(lens::resize(*d, wr, 0), Status::Unsupported, "resize");
    CHECK_L(lens::stat(*d, rd), Status::Unsupported, "stat");
    CHECK_L(lens::open_at(*d, rd, "x", Right::Read), Status::NotADirectory, "open_at below a stream");
    CHECK_L(lens::enumerate(*d, rd, &collect_name, nullptr), Status::NotADirectory, "enumerate a stream");
    CHECK_L(lens::at(*d, rd, Epoch(1)), Status::NoSuchEpoch, "no epoch views of a stream");
    CHECK(lens::sync(*d, wr).ok(), "sync on a stream is a no-op");

    CHECK(lens::write(*d, wr, 0, txt("tail")).ok(), "last bytes");
    CHECK(lens::close(*d, wr).ok(), "close the write end");
    CHECK(read_exact(*d, rd, "tail"), "the reader still gets what was written");
    auto eof = lens::read(*d, rd, 0, lens::Bytes(got, 8));
    CHECK(eof.ok() && eof.value() == 0, "then end-of-stream is Ok(0), not NotReady");
    CHECK(lens::read(*d, rd, 0, lens::Bytes(got, 8)).ok(), "and stays that way");
    lens::close_all(*d);

    CHECK(lens::make_pipe(*d, 64, &rd, &wr).ok(), "pipe");
    CHECK(lens::close(*d, rd).ok(), "close the read end");
    CHECK_L(lens::write(*d, wr, 0, txt("x")), Status::Closed, "writing with nobody reading");
    lens::close_all(*d);

    CHECK(lens::make_pipe(*d, 64, &rd, &wr).ok(), "pipe");
    Lens rd2 = lens::narrow(*d, rd, Right::Read).value();
    CHECK(lens::close(*d, rd).ok(), "close the original read lens");
    CHECK(lens::write(*d, wr, 0, txt("ok")).ok(), "the narrowed copy keeps the end open");
    CHECK(read_exact(*d, rd2, "ok"), "and reads");
    Lens none = lens::narrow(*d, rd2, Rights::none()).value();
    CHECK_L(lens::read(*d, none, 0, lens::Bytes(got, 8)), Status::Denied, "a narrowed-to-nothing lens cannot read");
    Lens rowr = lens::narrow(*d, wr, ALL, true).value();
    CHECK_L(lens::write(*d, rowr, 0, txt("x")), Status::ReadOnly, "a read-only lens cannot write");
    CHECK(lens::close(*d, none).ok() && lens::close(*d, rowr).ok(), "close copies");
    CHECK(lens::close(*d, rd2).ok(), "close the last read lens");
    CHECK_L(lens::write(*d, wr, 0, txt("x")), Status::Closed, "now the pipe is hung up");

    Domain* other = new_domain();
    Lens rd3, wr3;
    CHECK(lens::make_pipe(*d, 64, &rd3, &wr3).ok(), "pipe");
    Lens rdel = lens::narrow(*d, rd3, Right::Read | Right::Delegate).value();
    CHECK(lens::rights_of(*d, rdel).value() == (Right::Read | Right::Delegate), "a pipe end can be narrowed and keep Delegate");
    Lens moved = lens::delegate(*d, rdel, *other, Right::Read).value();
    lens::close(*d, rd3);
    lens::close(*d, rdel);
    CHECK(lens::write(*d, wr3, 0, txt("hi")).ok() && read_exact(*other, moved, "hi"), "a delegated end works in the other domain");
    lens::close_all(*other);
    free(other);

    lens::close_all(*d);
    Lens fill[lens::MAX_LENSES];
    for (u32 i = 0; i < lens::MAX_LENSES - 1; ++i) fill[i] = must_mint(*d, "ram:/", Right::Read);
    CHECK_L(lens::make_pipe(*d, 64, &rd, &wr), Status::TableFull, "a pipe needs two free slots");
    CHECK(rd.is_null() && wr.is_null() && lens::live_count(*d) == lens::MAX_LENSES - 1, "and takes none");
    lens::close_all(*d);
    free(d);
}

static void test_console_stream_and_await() {
    Domain* d = new_domain();
    lens::test_unseal();
    FakeConsole con;
    Lens in = lens::mint_stream(*d, &con, Right::Read | Right::Write).value();
    CHECK(!in.is_null() && con.refs == 1, "mint_stream wraps a kernel stream and takes a reference");
    CHECK(lens::kind_of(*d, in).value() == NodeKind::Stream, "a stream lens");
    char lab[32];
    lens::label_of(*d, in, lab, sizeof(lab));
    CHECK(same(lab, "console"), "labelled with the stream's name");
    CHECK_L(lens::read(*d, in, 0, lens::Bytes(reinterpret_cast<u8*>(lab), 4)), Status::NotReady, "no input yet");
    con.type("ls\n");
    char b[16];
    auto r = lens::read(*d, in, 0, lens::Bytes(reinterpret_cast<u8*>(b), 16));
    CHECK(r.ok() && r.value() == 3 && mem::compare(b, "ls\n", 3) == 0, "typed input is read");
    CHECK(lens::write(*d, in, 0, txt("$ ")).value() == 2 && con.out_len == 2 && con.out[0] == '$', "output is written");
    Lens dup = lens::narrow(*d, in, Right::Write).value();
    CHECK(con.refs == 2, "a copy takes a reference");
    lens::close(*d, dup);
    CHECK(con.refs == 1, "closing it drops it");
    lens::close(*d, in);
    CHECK(con.refs == 0 && con.releases == 2, "every reference is returned");
    lens::seal();
    CHECK_L(lens::mint_stream(*d, &con, Right::Read), Status::Denied, "mint_stream after seal");
    lens::test_unseal();
    CHECK_L(lens::mint_stream(*d, nullptr, Right::Read), Status::InvalidPath, "mint_stream of nothing");

    Lens rd, wr;
    lens::make_pipe(*d, 64, &rd, &wr);
    lens::Wait w[2];
    w[0] = lens::Wait{rd, Ready::Readable, {}};
    w[1] = lens::Wait{wr, Ready::Writable, {}};
    auto a0 = lens::await(*d, w, 2, 0);
    CHECK(a0.ok() && a0.value() == 1 && !w[0].got.any() && w[1].got.has(Ready::Writable), "poll: an empty pipe is writable, not readable");
    lens::write(*d, wr, 0, txt("x"));
    auto a1 = lens::await(*d, w, 2, 0);
    CHECK(a1.ok() && a1.value() == 2 && w[0].got.has(Ready::Readable) && w[1].got.has(Ready::Writable), "poll: both ready");
    char ch[4];
    lens::read(*d, rd, 0, lens::Bytes(reinterpret_cast<u8*>(ch), 4));

    lens::Wait only_read[1] = {lens::Wait{rd, Ready::Readable, {}}};
    u64 t0 = host_ms();
    auto to = lens::await(*d, only_read, 1, 60);
    u64 waited = host_ms() - t0;
    CHECK(to.status() == Status::TimedOut && !only_read[0].got.any(), "nothing arrives: TimedOut");
    CHECK(waited >= 55 && waited < 400, "after about the time asked for");
    CHECK_L(lens::await(*d, only_read, 1, 0), Status::TimedOut, "a zero timeout polls and times out");

    struct Feeder { Domain* d; Lens wr; };
    static Feeder feeder;
    feeder = Feeder{d, wr};
    auto feed = [](void* p) -> void* {
        Feeder* f = static_cast<Feeder*>(p);
        usleep(50000);
        lens::write(*f->d, f->wr, 0, txt("late"));
        return nullptr;
    };
    pthread_t th;
    pthread_create(&th, nullptr, feed, &feeder);
    t0 = host_ms();
    auto woke = lens::await(*d, only_read, 1, 5000);
    waited = host_ms() - t0;
    pthread_join(th, nullptr);
    CHECK(woke.ok() && only_read[0].got.has(Ready::Readable), "a late write wakes the wait");
    CHECK(waited >= 40 && waited < 2000, "well before the timeout");
    lens::read(*d, rd, 0, lens::Bytes(reinterpret_cast<u8*>(ch), 4));

    lens::close(*d, wr);
    auto hang = lens::await(*d, only_read, 1, 1000);
    CHECK(hang.ok() && only_read[0].got.has(Ready::Hangup) && only_read[0].got.has(Ready::Readable), "closing the writer reports Hangup and Readable");
    lens::close(*d, rd);

    lens::make_pipe(*d, 64, &rd, &wr);
    lens::Wait bad[1] = {lens::Wait{wr, Ready::Readable, {}}};
    CHECK_L(lens::await(*d, bad, 1, 0), Status::Denied, "waiting to read a write-only lens");
    lens::Wait bad2[1] = {lens::Wait{rd, Ready::Writable, {}}};
    CHECK_L(lens::await(*d, bad2, 1, 0), Status::Denied, "waiting to write a read-only lens");
    lens::close(*d, rd);
    lens::Wait stale[1] = {lens::Wait{rd, Ready::Readable, {}}};
    CHECK_L(lens::await(*d, stale, 1, 0), Status::StaleLens, "waiting on a closed lens");
    Lens file = must_mint(*d, "ram:/proj/readme.txt", Right::Read);
    lens::Wait filew[1] = {lens::Wait{file, Ready::Readable, {}}};
    CHECK_L(lens::await(*d, filew, 1, 0), Status::Unsupported, "readiness is for streams, not files");
    CHECK_L(lens::await(*d, nullptr, 1, 0), Status::InvalidPath, "no waits");
    CHECK_L(lens::await(*d, bad, 0, 0), Status::InvalidPath, "zero waits");
    lens::Wait many[33];
    for (auto& m : many) m = lens::Wait{wr, Ready::Writable, {}};
    CHECK_L(lens::await(*d, many, 33, 0), Status::InvalidPath, "too many waits");

    FakeConsole con2;
    Lens c2 = lens::mint_stream(*d, &con2, Right::Read | Right::Write).value();
    lens::Wait cw[1] = {lens::Wait{c2, Ready::Readable, {}}};
    CHECK_L(lens::await(*d, cw, 1, 20), Status::TimedOut, "no keys pressed");
    con2.type("a");
    CHECK(lens::await(*d, cw, 1, 20).ok() && cw[0].got.has(Ready::Readable), "a key press is readiness");

    lens::close_all(*d);
    free(d);
}

using lens::text::Cursor;
using NS = nebula::Status;

static bool at_text(Cursor& c, const char* want) {
    char b[nebula::path::TEXT_MAX];
    lens::text::location_text(c, b, sizeof(b));
    return cstr::compare(b, want) == 0;
}

static bool reads_as(Cursor& c, const char* path, const char* want) {
    nebula::Blob b;
    if (lens::text::read_file(c, path, &b) != NS::Ok) return false;
    bool same = b.size == cstr::length(want) && (b.size == 0 || mem::compare(b.data, want, b.size) == 0);
    nebula::release(b);
    return same;
}

static void test_cursor() {
    lens::test_unseal();
    CHECK(lens::mint_boot_roots().ok(), "boot roots for every realm");
    lens::seal();
    CHECK(lens::boot_root("ram") != Lens() && lens::boot_root("disk") != Lens() && lens::boot_root("sys") != Lens(), "roots by realm name");
    CHECK(lens::boot_root("nowhere").is_null(), "no root for an unknown realm");
    CHECK_L(lens::mint(lens::boot_domain(), "ram:/", ALL), Status::Denied, "no minting after seal");

    Cursor* cp = static_cast<Cursor*>(calloc(1, sizeof(Cursor)));
    Cursor& c = *cp;
    CHECK(lens::text::init(c, "ram").ok(), "cursor init");
    u32 roots = c.root_count;
    CHECK(roots == 6 && lens::live_count(c.domain) == roots, "one delegated root per realm, nothing else");
    CHECK(at_text(c, "ram:/"), "starts at the realm root");

    CHECK(lens::text::change_location(c, "proj/src") == NS::Ok && at_text(c, "ram:/proj/src"), "cd below the root");
    CHECK(c.depth == 2 && lens::live_count(c.domain) == roots + 2, "a lens per component");
    CHECK(reads_as(c, "main.c", "int main;"), "relative read starts at the current lens");
    CHECK(reads_as(c, "deep/leaf.txt", "leaf"), "and descends");
    CHECK(reads_as(c, "../readme.txt", "hello lens"), ".. goes up the parent stack");
    CHECK(reads_as(c, "../../proj/readme.txt", "hello lens"), "up and back down");
    CHECK(reads_as(c, "ram:/proj/readme.txt", "hello lens") && reads_as(c, "/proj/readme.txt", "hello lens"), "realm prefix and absolute start at the root");
    CHECK(reads_as(c, "./deep/../main.c", "int main;"), ".. inside the text collapses before any lens is touched");
    CHECK(reads_as(c, "disk:/proj/readme.txt", "hello lens") && at_text(c, "ram:/proj/src"), "another realm by prefix leaves the location alone");
    CHECK(lens::live_count(c.domain) == roots + 2, "reads leave no lenses open");

    CHECK(lens::text::change_location(c, "../../../../..") == NS::Ok && at_text(c, "ram:/"), "cd far above the root stops at the root");
    CHECK(c.depth == 0 && lens::live_count(c.domain) == roots, "stack unwound");
    CHECK(lens::text::change_location(c, "proj/src/deep") == NS::Ok && c.depth == 3, "three deep");
    CHECK(lens::text::change_location(c, "../..") == NS::Ok && at_text(c, "ram:/proj") && c.depth == 1, "cd .. twice");
    CHECK(lens::live_count(c.domain) == roots + 1, "the stack shrank with it");
    CHECK(lens::text::change_location(c, "src") == NS::Ok && c.depth == 2, "back down");

    u32 live = lens::live_count(c.domain);
    CHECK(lens::text::change_location(c, "main.c") == NS::NotADirectory && at_text(c, "ram:/proj/src"), "cd into a file");
    CHECK(lens::text::change_location(c, "nodir/x") == NS::NotFound && at_text(c, "ram:/proj/src"), "cd into nothing");
    CHECK(lens::text::change_location(c, "deep/nope") == NS::NotFound && lens::live_count(c.domain) == live, "half a path leaves no lens behind");
    CHECK(lens::text::change_location(c, "nowhere:/x") == NS::NoRealm, "unknown realm");
    CHECK(lens::text::change_location(c, "") == NS::InvalidPath, "empty path");

    CHECK(lens::text::change_location(c, "disk:/proj/src/deep") == NS::Ok && at_text(c, "disk:/proj/src/deep") && c.depth == 3, "cd to another realm");
    CHECK(lens::live_count(c.domain) == roots + 3, "the old realm's levels are closed");
    CHECK(reads_as(c, "leaf.txt", "leaf") && reads_as(c, "../main.c", "int main;"), "relative paths now resolve in disk");
    CHECK(lens::text::change_location(c, "sys:/") == NS::Ok && c.depth == 0 && lens::live_count(c.domain) == roots, "sys root");
    nebula::Blob vb;
    CHECK(lens::text::read_file(c, "version", &vb) == NS::Ok && vb.size > 0, "a probe reads through the cursor");
    nebula::release(vb);
    CHECK(lens::text::write_file(c, "version", "x", 1) == NS::ReadOnly, "and refuses writes");
    CHECK(lens::text::change_location(c, "ram:/proj") == NS::Ok, "back to ram");

    CHECK(reads_as(c, "./a:b", "colon name"), "relative ./a:b");
    CHECK(lens::text::change_location(c, "ram:/") == NS::Ok && reads_as(c, "proj/a:b", "colon name"), "and below a directory");
    CHECK(lens::text::change_location(c, "proj") == NS::Ok, "reenter");

    CHECK(lens::text::write_file(c, "note.txt", "one", 3) == NS::Ok && reads_as(c, "note.txt", "one"), "write_file");
    CHECK(lens::text::append_file(c, "note.txt", "+two", 4) == NS::Ok && reads_as(c, "note.txt", "one+two"), "append_file");
    CHECK(lens::text::append_file(c, "fresh.txt", "new", 3) == NS::Ok && reads_as(c, "fresh.txt", "new"), "append_file creates");
    CHECK(lens::text::write_file(c, "src", "x", 1) == NS::IsADirectory, "write_file over a directory");
    CHECK(lens::text::write_file(c, "ram:/", "x", 1) == NS::IsADirectory, "write_file at a root");
    CHECK(lens::text::append_file(c, "src", "x", 1) == NS::IsADirectory, "append to a directory");
    CHECK(lens::text::write_file(c, "nodir/x", "x", 1) == NS::NotFound, "write below a missing directory");
    CHECK(lens::text::make_dir(c, "made") == NS::Ok && lens::text::make_dir(c, "made") == NS::Exists, "make_dir then Exists");
    CHECK(lens::text::make_dir(c, "ram:/") == NS::Exists, "the root exists");
    CHECK(lens::text::make_path(c, "p/q/r") == NS::Ok && lens::text::make_path(c, "p/q/r") == NS::Ok, "make_path is idempotent");
    CHECK(lens::text::make_path(c, "note.txt/x") == NS::NotADirectory, "make_path through a file");
    CHECK(lens::text::remove(c, "ram:/") == NS::Denied && lens::text::remove_tree(c, "/", nullptr) == NS::Denied, "roots cannot be removed");
    CHECK(lens::text::remove(c, "src") == NS::NotEmpty, "remove refuses a full directory");
    CHECK(lens::text::rename(c, "note.txt", "made/note2.txt", 0) == NS::Ok && reads_as(c, "made/note2.txt", "one+two"), "rename into a directory");
    CHECK(lens::text::rename(c, "fresh.txt", "made/note2.txt", 0) == NS::Exists, "rename onto a file");
    CHECK(lens::text::rename(c, "fresh.txt", "made/note2.txt", nebula::RENAME_REPLACE) == NS::Ok && reads_as(c, "made/note2.txt", "new"), "rename with replace");
    CHECK(lens::text::rename(c, "src", "src/deep/inside", 0) == NS::Unsupported, "a folder cannot move into itself");
    CHECK(lens::text::rename(c, "src", "src", 0) == NS::Ok, "moving a name onto itself is a no-op");
    CHECK(lens::text::rename(c, "ram:/", "x", 0) == NS::Denied, "the root cannot be renamed");
    CHECK(lens::text::rename(c, "readme.txt", "disk:/proj/moved.txt", 0) == NS::CrossRealm, "rename across realms");
    u32 copied = 0;
    CHECK(lens::text::copy_tree(c, "src", "copy", &copied) == NS::Ok && copied == 2 && reads_as(c, "copy/deep/leaf.txt", "leaf"), "copy_tree");
    CHECK(lens::text::copy_tree(c, "src", "src/deep/again", &copied) == NS::Unsupported, "copy_tree into itself");
    CHECK(lens::text::copy_tree(c, "src", "disk:/proj/copied-src", &copied) == NS::Ok && reads_as(c, "disk:/proj/copied-src/main.c", "int main;"), "copy_tree across realms");
    CHECK(lens::text::copy_tree(c, "readme.txt", "readme2.txt", &copied) == NS::Ok && copied == 1, "copy_tree of a file is a file copy");
    CHECK(lens::text::copy_file(c, "readme.txt", "made") == NS::Ok && reads_as(c, "made/readme.txt", "hello lens"), "copy_file into a directory");
    CHECK(lens::text::copy_file(c, "src", "x") == NS::IsADirectory, "copy_file of a directory");
    CHECK(lens::text::copy_file(c, "readme.txt", "readme.txt") == NS::Exists, "copy onto itself");
    CHECK(lens::text::remove_tree(c, "copy", &copied) == NS::Ok && copied == 4, "remove_tree counts what it removed");
    CHECK(lens::text::remove_tree(c, "copy", nullptr) == NS::NotFound, "and then it is gone");
    CHECK(lens::live_count(c.domain) == roots + c.depth, "every operation closed what it opened");

    CHECK(lens::text::change_location(c, "ram:/proj/made") == NS::Ok, "into made");
    CHECK(lens::text::make_path(c, ".") == NS::Ok && lens::text::make_dir(c, ".") == NS::Exists, "mkdir of the current directory");
    CHECK(lens::text::write_file(c, ".", "x", 1) == NS::IsADirectory, "write_file over the current directory");
    CHECK(lens::text::rename(c, ".", "../moved", 0) == NS::Ok && at_text(c, "ram:/proj/moved"), "renaming the current directory: the location follows");
    CHECK(reads_as(c, "note2.txt", "new") && c.depth == 2, "the lenses followed the node");
    CHECK(lens::text::make_dir(c, "q") == NS::Ok, "still usable");
    CHECK(lens::text::change_location(c, "q") == NS::Ok && at_text(c, "ram:/proj/moved/q"), "one level down");
    CHECK(lens::text::rename(c, "..", "../../../proj2", 0) == NS::Ok && at_text(c, "ram:/proj2/q"), "renaming an ancestor of the current directory");
    CHECK(lens::text::remove_tree(c, "..", nullptr) == NS::Ok && at_text(c, "ram:/") && c.depth == 0, "removing an ancestor climbs to what survives");
    nebula::Info untouched;
    CHECK(lens::text::stat(c, "proj/readme.txt", &untouched) == NS::Ok, "and the rest of the tree is untouched");
    CHECK(lens::live_count(c.domain) == roots, "no lens left behind");
    CHECK(lens::text::change_location(c, "proj") == NS::Ok, "back into proj");

    nebula::Listing l;
    CHECK(lens::text::list(c, ".", &l, true) == NS::Ok && l.count >= 4, "list");
    bool sorted = true;
    for (u32 i = 1; i < l.count; ++i) {
        bool prev_dir = l.items[i - 1].kind == nebula::Kind::Dir, dir = l.items[i].kind == nebula::Kind::Dir;
        if (!prev_dir && dir) sorted = false;
        if (prev_dir == dir && cstr::compare(l.items[i - 1].name, l.items[i].name) >= 0) sorted = false;
    }
    CHECK(sorted, "folders first, then by name");
    nebula::release(l);
    CHECK(lens::text::list(c, "readme.txt", &l, false) == NS::NotADirectory, "list of a file");

    lens::text::StatRow row;
    CHECK(lens::text::stat_row(c, "src/../src/main.c", &row) == NS::Ok && row.info.size == 9 && row.writable, "stat_row");
    char rt[64];
    nebula::path::to_text(row.path, rt, sizeof(rt));
    CHECK(cstr::compare(rt, "ram:/proj/src/main.c") == 0 && cstr::compare(row.realm, "ram") == 0, "stat_row gives the canonical path and realm");
    CHECK(lens::text::stat_row(c, "sys:/version", &row) == NS::Ok && !row.writable, "a read-only realm");

    CHECK(lens::text::change_location(c, "disk:/proj/src/deep") == NS::Ok, "into the disk");
    nebula::disk_set_mounted(true);
    CHECK(reads_as(c, "leaf.txt", "leaf") && at_text(c, "disk:/proj/src/deep") && c.depth == 3, "a remount rebuilds the stack and keeps the place");
    CHECK(nebula::remove_tree(g_sess, "disk:/proj/src/deep", nullptr) == NS::Ok, "the directory vanishes underneath");
    nebula::disk_set_mounted(true);
    CHECK(lens::text::stat(c, ".", &row.info) == NS::Ok && at_text(c, "disk:/proj/src") && c.depth == 2, "the deepest surviving ancestor becomes the location");
    nebula::disk_set_mounted(false);
    CHECK(lens::text::list(c, ".", &l, false) == NS::Unavailable, "an unmounted disk is Unavailable");
    CHECK(reads_as(c, "ram:/proj/readme.txt", "hello lens"), "other realms are unaffected");
    nebula::disk_set_mounted(true);
    CHECK(lens::text::list(c, ".", &l, false) == NS::Ok && at_text(c, "disk:/proj/src"), "and it comes back");
    nebula::release(l);
    fixture("disk");

    CHECK(lens::text::change_location(c, "ram:/proj") == NS::Ok, "to ram");
    auto g = lens::text::grant(c, "src", Right::Read | Right::Enumerate);
    CHECK(g.ok(), "grant");
    Domain* task = new_domain();
    auto given = lens::delegate(c.domain, g.value(), *task, Right::Read | Right::Enumerate);
    CHECK(given.ok(), "delegated to the task");
    lens::close(c.domain, g.value());
    Lens t = given.value();
    CHECK(lens::rights_of(*task, t).value() == (Right::Read | Right::Enumerate), "the task holds exactly what was granted");
    Lens tf = lens::open_at(*task, t, "main.c", Right::Read).value();
    u8 buf[16];
    auto got = lens::read(*task, tf, 0, lens::Bytes(buf, sizeof(buf)));
    CHECK(got.ok() && got.value() == 9 && mem::compare(buf, "int main;", 9) == 0, "it can read below the grant");
    CHECK_L(lens::open_at(*task, t, "../readme.txt", Right::Read), Status::Escape, "and cannot name anything above it");
    CHECK_L(lens::open_at(*task, t, "ram:/proj", Right::Read), Status::Escape, "nor by realm prefix");
    CHECK_L(lens::open_at(*task, t, "main.c", Right::Write), Status::Denied, "nor ask for more rights");
    CHECK_L(lens::write(*task, tf, 0, txt("x")), Status::Denied, "nor write");
    CHECK_L(lens::delegate(*task, t, *task, Right::Read), Status::Denied, "nor hand it on: no Delegate");
    auto ro = lens::text::grant(c, "src", Right::Read | Right::Write, true);
    CHECK(ro.ok() && lens::read_only_of(c.domain, ro.value()).value(), "a read-only grant stays read-only");
    CHECK_L(lens::write(c.domain, lens::open_at(c.domain, ro.value(), "main.c", Right::Write).value(), 0, txt("x")), Status::ReadOnly, "even with Write");
    lens::close(c.domain, ro.value());
    CHECK_L(lens::text::grant(c, "nodir", Right::Read), Status::NotFound, "grant of nothing");
    lens::close_all(*task);
    free(task);

    lens::text::close(c);
    CHECK(lens::live_count(c.domain) == 0, "close returns every lens");
    free(cp);
}

int main() {
    g_sectors = 131072;
    g_disk = static_cast<u8*>(calloc(g_sectors, 512));
    stellar::init(0);
    CHECK(stellar::format(g_sectors), "format");
    nebula::disk_set_mounted(true);
    CHECK(nebula::register_realm(nebula::ram_realm()), "register ram");
    CHECK(nebula::register_realm(nebula::disk_realm()), "register disk");
    CHECK(nebula::register_realm(nebula::snap_realm()), "register snap");
    CHECK(nebula::register_realm(&g_probe), "register probe");
    CHECK(nebula::register_realm(&g_count), "register count");
    CHECK(nebula::register_realm(nebula::sys_realm()), "register sys");
    nebula::init_session(g_sess, "ram");

    fixture("ram");
    fixture("disk");

    printf("-- mint\n");
    test_mint();
    printf("-- open_at\n");
    test_open_at();
    printf("-- rights\n");
    test_rights_flow();
    printf("-- delegate\n");
    test_delegate();
    printf("-- stale lenses and table limits\n");
    test_stale();
    test_table_limits();
    test_zero_memory_domain();
    printf("-- operations on ram\n");
    ops_suite("ram");
    printf("-- operations on disk\n");
    ops_suite("disk");
    printf("-- cross-realm and probes\n");
    test_cross_realm_and_probe();
    printf("-- traffic matches Nebula\n");
    test_same_traffic_as_nebula();
    printf("-- epoch views\n");
    test_epochs();
    test_epoch_limits();
    printf("-- sys probes\n");
    test_sys_probes();
    printf("-- pipes and streams\n");
    test_pipes();
    test_console_stream_and_await();
    printf("-- cursor\n");
    test_cursor();
    printf("-- batch\n");
    test_batch_basics();
    test_batch_failure_injection();
    test_batch_group_commit();
    test_batch_holds_the_lock();
    test_batch_ram();
    test_batch_spans_realms();

    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
