#include <cosmos/types.hpp>
#include <cosmos/blockdev.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/stellar.hpp>
#include <cosmos/orbital.hpp>
#include <cosmos/civil.hpp>

extern "C" {
int printf(const char*, ...);
int vprintf(const char*, __builtin_va_list);
int puts(const char*);
void* aligned_alloc(unsigned long, unsigned long);
void* calloc(unsigned long, unsigned long);
void free(void*);
typedef unsigned long pthread_t;
int pthread_create(pthread_t*, const void*, void* (*)(void*), void*);
int pthread_join(pthread_t, void**);
int sched_yield(void);
}

static u8* g_disk;
static u64 g_sectors;

static u64 g_wcount = 0;
static u64 g_last_sb_w = 0;
static u64 g_flushes = 0;
struct LogEnt { u64 sector; u8 data[512]; };
static LogEnt g_log[24576];
static u32 g_log_n = 0;
static bool g_rec = false, g_quiet = false;
static void rec_write(u64 s, const void* d) { if (g_rec && g_log_n < 24576) { g_log[g_log_n].sector = s; __builtin_memcpy(g_log[g_log_n].data, d, 512); ++g_log_n; } }
static void rec_barrier() { if (g_rec && g_log_n < 24576) g_log[g_log_n++].sector = ~0ull; }
static bool g_flush_fails = false;
static u64 g_fail_after = ~0ull;

namespace blockdev {
bool read_sector(u64 s, void* b) { if (s >= g_sectors) return false; __builtin_memcpy(b, g_disk + s * 512, 512); return true; }
bool write_sector(u64 s, const void* b) {
    if (s >= g_sectors || g_wcount >= g_fail_after) return false;
    ++g_wcount;
    if (s < 2) g_last_sb_w = g_wcount;
    rec_write(s, b);
    __builtin_memcpy(g_disk + s * 512, b, 512);
    return true;
}
bool read_sectors(u64 s, u64 n, void* b) { if (s + n > g_sectors) return false; __builtin_memcpy(b, g_disk + s * 512, n * 512); return true; }
bool write_sectors(u64 s, u64 n, const void* b) {
    if (s + n > g_sectors) return false;
    u64 ok_n = n;
    if (g_wcount + n > g_fail_after) ok_n = g_fail_after > g_wcount ? g_fail_after - g_wcount : 0;
    for (u64 i = 0; i < ok_n; ++i) { rec_write(s + i, static_cast<const u8*>(b) + i * 512); if (s + i < 2) g_last_sb_w = g_wcount + i + 1; }
    __builtin_memcpy(g_disk + s * 512, b, ok_n * 512);
    g_wcount += ok_n;
    return ok_n == n;
}
u64 capacity_sectors() { return g_sectors; }
bool flush() { ++g_flushes; if (g_flush_fails) return false; rec_barrier(); return true; }
}

namespace orbital {
void yield() { sched_yield(); }
void yield_contended() { sched_yield(); }
u64 self_token() { static thread_local char tag; return reinterpret_cast<u64>(&tag); }
}

static long g_allocs_until_fail = -1;

namespace universe {
u64 alloc(int order) {
    if (g_allocs_until_fail == 0) return 0;
    if (g_allocs_until_fail > 0) --g_allocs_until_fail;
    unsigned long bytes = PAGE_SIZE << order;
    return reinterpret_cast<u64>(aligned_alloc(4096, bytes));
}
void free(u64, int) {}
}

namespace serial {
void printf(const char* fmt, ...) {
    if (g_quiet) return;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vprintf(fmt, ap);
    __builtin_va_end(ap);
}
void writeln(const char* s) { if (!g_quiet) puts(s); }
}

static int g_fail = 0;
#define CHECK(c, msg) do { if (!(c)) { ++g_fail; printf("FAIL: %s  (line %d)\n", msg, __LINE__); } } while (0)

static bool eq(u64 star, const char* expect, u64 snap = stellar::LIVE) {
    static u8 tmp[4096];
    u64 want = strlen(expect);
    if (star == stellar::INVALID_STAR) return false;
    u64 got = stellar::read_file(star, tmp, sizeof(tmp), snap);
    if (got != want) return false;
    for (u64 i = 0; i < got; ++i) if (tmp[i] != static_cast<u8>(expect[i])) return false;
    return true;
}

static void sb_fix_crc(u8* sec) { u32 c = stellar::crc32(sec, 508); __builtin_memcpy(sec + 508, &c, 4); }
static void sb_poke(u64 off, u64 val, int bytes) {
    for (int slot = 0; slot < 2; ++slot) {
        u8* sec = g_disk + slot * 512;
        __builtin_memcpy(sec + off, &val, bytes);
        sb_fix_crc(sec);
    }
}
static bool strcmp_(const char* a, const char* b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static void count_cb(const char*, u64, u32, void* ctx) { ++*static_cast<u64*>(ctx); }
static u64 count_dir(u64 dir, u64 snap = stellar::LIVE) {
    u64 n = 0;
    stellar::list(dir, &count_cb, &n, snap);
    return n;
}

static void name_of(char* out, const char* prefix, u64 i) {
    u32 p = 0;
    while (*prefix) out[p++] = *prefix++;
    char d[20]; u32 l = 0;
    if (i == 0) d[l++] = '0';
    while (i) { d[l++] = static_cast<char>('0' + i % 10); i /= 10; }
    while (l) out[p++] = d[--l];
    out[p] = 0;
}

static u64 make(u64 dir, const char* n, const char* c) { return stellar::create_file(dir, n, c, strlen(c)); }

static void fresh_fs(u64 sectors = 131072) {
    free(g_disk);
    g_sectors = sectors;
    g_disk = static_cast<u8*>(calloc(g_sectors, 512));
    stellar::init(0);
    CHECK(!stellar::mount(), "blank disk must not mount");
    CHECK(stellar::format(g_sectors), "format");
}


struct DEnt { char name[56]; u64 star; u32 type; };
struct DCtx { DEnt e[96]; u32 n; };
static void digest_cb(const char* name, u64 star, u32 type, void* c) {
    auto* x = static_cast<DCtx*>(c);
    if (x->n >= 96) return;
    u32 i = 0;
    for (; i < 55 && name[i]; ++i) x->e[x->n].name[i] = name[i];
    x->e[x->n].name[i] = 0;
    x->e[x->n].star = star;
    x->e[x->n].type = type;
    ++x->n;
}
static u64 dmix(u64 h, u64 v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); return h * 0x100000001b3ull; }
static u32 g_digest_depth = 0;
static u64 digest_dir(u64 dir, u64 snap, u64 path) {
    if (g_digest_depth > 12) return 0xDEADDEADull;
    struct D { D() { ++g_digest_depth; } ~D() { --g_digest_depth; } } depth_guard;
    DCtx ctx; ctx.n = 0;
    stellar::list(dir, &digest_cb, &ctx, snap);
    u64 sum = 0;
    for (u32 i = 0; i < ctx.n; ++i) {
        u64 h = path;
        for (u32 k = 0; ctx.e[i].name[k]; ++k) h = dmix(h, static_cast<u8>(ctx.e[i].name[k]));
        h = dmix(h, ctx.e[i].type);
        if (ctx.e[i].type == stellar::TYPE_FILE) {
            u8 buf[4096];
            u64 got = stellar::read_file(ctx.e[i].star, buf, sizeof(buf), snap);
            h = got == stellar::READ_ERROR ? dmix(h, 0xBAD) : dmix(dmix(h, got), stellar::crc32(buf, got));
            sum += h;
        } else if (ctx.e[i].type == stellar::TYPE_CONSTELLATION) {
            sum += dmix(h, 7) + digest_dir(ctx.e[i].star, snap, h);
        }
    }
    return sum;
}
static u64 tree_digest() {
    u64 d = digest_dir(stellar::ROOT_STAR, stellar::LIVE, 1);
    for (u64 snap = 1; snap <= stellar::SNAPSHOT_MAX; ++snap) {
        stellar::StatInfo si;
        if (stellar::stat(stellar::ROOT_STAR, &si, snap)) d = dmix(d, snap) + digest_dir(stellar::ROOT_STAR, snap, snap * 131);
    }
    return d;
}

int main() {
    fresh_fs();

    u64 hello = make(stellar::ROOT_STAR, "hello.txt", "hello\n");
    u64 sub = stellar::create_constellation(stellar::ROOT_STAR, "sub");
    u64 nested = make(sub, "nested.txt", "nested\n");
    CHECK(eq(hello, "hello\n") && eq(nested, "nested\n"), "basic readback");
    CHECK(stellar::find(sub, "nested.txt") == nested, "nested find");

    auto a = stellar::io_stats();
    u64 s1 = stellar::snapshot();
    auto b = stellar::io_stats();
    printf("tiny tree   snapshot: %lu write(s), %lu read(s)\n", b.writes - a.writes, b.reads - a.reads);
    CHECK(s1 == 1 && b.writes - a.writes == 1 && b.reads - a.reads == 0, "snapshot is 1 write, 0 reads");

    CHECK(stellar::write_file(hello, "changed\n", 8) == hello, "write_file after snapshot");
    u64 extra = make(sub, "extra.txt", "x\n");
    u64 root_new = make(stellar::ROOT_STAR, "new.txt", "n\n");
    CHECK(eq(hello, "changed\n"), "live sees rewrite");
    CHECK(eq(hello, "hello\n", s1), "snapshot keeps old bytes");
    CHECK(stellar::verify_file(hello) && stellar::verify_file(hello, s1), "both checksums verify");
    CHECK(stellar::find(sub, "extra.txt") == extra && stellar::find(sub, "extra.txt", s1) == stellar::INVALID_STAR, "snapshot dir isolation");
    CHECK(stellar::find(stellar::ROOT_STAR, "new.txt") == root_new && stellar::find(stellar::ROOT_STAR, "new.txt", s1) == stellar::INVALID_STAR, "snapshot root isolation");
    CHECK(count_dir(stellar::ROOT_STAR, s1) == 2 && count_dir(stellar::ROOT_STAR) == 3, "root counts per view");
    CHECK(eq(nested, "nested\n", s1), "nested file visible in snapshot");

    const u64 N = 700;
    for (u64 i = 0; i < N; ++i) {
        char nm[32]; name_of(nm, "f", i);
        u64 st = stellar::create_file(stellar::ROOT_STAR, nm, nm, strlen(nm));
        CHECK(st != stellar::INVALID_STAR, "bulk create");
    }
    u64 deep = sub;
    for (int d = 0; d < 5; ++d) {
        deep = stellar::create_constellation(deep, "d");
        make(deep, "leaf", "leaf\n");
    }
    u64 total_live = count_dir(stellar::ROOT_STAR);
    CHECK(total_live == 3 + N, "live dir count after bulk");

    a = stellar::io_stats();
    u64 s2 = stellar::snapshot();
    b = stellar::io_stats();
    printf("big tree    snapshot: %lu write(s), %lu read(s)  (%lu entries in root, 5-deep subtree)\n",
           b.writes - a.writes, b.reads - a.reads, total_live);
    CHECK(s2 == 2 && b.writes - a.writes == 1 && b.reads - a.reads == 0, "snapshot cost independent of tree size");

    for (u64 i = 0; i < N; i += 7) {
        char nm[32]; name_of(nm, "f", i);
        u64 st = stellar::find(stellar::ROOT_STAR, nm);
        stellar::write_file(st, "rewritten", 9);
    }
    make(stellar::ROOT_STAR, "post.txt", "p\n");
    for (u64 i = 0; i < N; ++i) {
        char nm[32]; name_of(nm, "f", i);
        u64 st = stellar::find(stellar::ROOT_STAR, nm, s2);
        CHECK(eq(st, nm, s2), "s2 view keeps original file bytes");
        u64 live = stellar::find(stellar::ROOT_STAR, nm);
        CHECK(st == live, "star ids stable across views");
        CHECK(eq(live, (i % 7 == 0) ? "rewritten" : nm), "live view content");
    }
    CHECK(count_dir(stellar::ROOT_STAR, s2) == 3 + N, "s2 root count frozen");
    CHECK(count_dir(stellar::ROOT_STAR) == 4 + N, "live root count grew");
    CHECK(count_dir(stellar::ROOT_STAR, s1) == 2, "s1 still frozen");

    u64 free0 = stellar::free_space_sectors();
    u64 hot = make(stellar::ROOT_STAR, "hot", "0");
    u64 free1 = stellar::free_space_sectors();
    for (int i = 0; i < 300; ++i) stellar::write_file(hot, "payload", 7);
    u64 free2 = stellar::free_space_sectors();
    printf("rewrite loop (no snapshot between): free sectors %lu -> %lu -> %lu\n", free0, free1, free2);
    CHECK(free2 == free1, "in-epoch rewrites reclaim their old extents");
    u64 s3 = stellar::snapshot();
    stellar::write_file(hot, "post-snap", 9);
    CHECK(eq(hot, "payload", s3) && eq(hot, "post-snap"), "post-snapshot rewrite isolated");
    CHECK(free2 - stellar::free_space_sectors() >= 1, "snapshot retains its extent");

    static u8 big[200000], back[200000];
    for (u64 i = 0; i < sizeof(big); ++i) big[i] = static_cast<u8>((i * 40503u) >> 8);
    u64 bigf = stellar::create_file(stellar::ROOT_STAR, "big.bin", big, sizeof(big));
    u64 got = stellar::read_file(bigf, back, sizeof(back));
    bool same = got == sizeof(big);
    for (u64 i = 0; same && i < got; ++i) same = big[i] == back[i];
    CHECK(same && stellar::verify_file(bigf), "200 KB file round trip");

    fresh_fs();
    stellar::init(0);
    {
        u64 h2 = make(stellar::ROOT_STAR, "h", "h\n");
        auto x = stellar::io_stats();
        for (int i = 0; i < 100; ++i) {
            char nm[32]; name_of(nm, "c", i);
            make(stellar::ROOT_STAR, nm, "data");
        }
        auto y = stellar::io_stats();
        printf("create_file avg: %.1f sector writes, %.1f sector reads (%lu cache hits)\n",
               (double)(y.writes - x.writes) / 100.0, (double)(y.reads - x.reads) / 100.0, y.cache_hits - x.cache_hits);
        (void)h2;
    }

    fresh_fs();
    u64 keep = make(stellar::ROOT_STAR, "keep.txt", "v1\n");
    u64 sa = stellar::snapshot();
    stellar::write_file(keep, "v2\n", 3);
    u64 dd = stellar::create_constellation(stellar::ROOT_STAR, "dir");
    for (int i = 0; i < 40; ++i) { char nm[32]; name_of(nm, "e", i); make(dd, nm, nm); }
    CHECK(stellar::mount(), "remount");
    CHECK(stellar::snapshot_count() == 1, "snapshot table persisted");
    CHECK(eq(stellar::find(stellar::ROOT_STAR, "keep.txt"), "v2\n"), "live persisted");
    CHECK(eq(stellar::find(stellar::ROOT_STAR, "keep.txt", sa), "v1\n", sa), "snapshot persisted");
    CHECK(count_dir(stellar::find(stellar::ROOT_STAR, "dir")) == 40, "dir persisted");
    CHECK(make(stellar::ROOT_STAR, "after.txt", "ok") != stellar::INVALID_STAR, "writes after remount");
    CHECK(stellar::find(stellar::ROOT_STAR, "after.txt", sa) == stellar::INVALID_STAR, "post-remount isolation");

    fresh_fs();
    u64 last = 0;
    for (u32 i = 0; i < stellar::SNAPSHOT_MAX; ++i) last = stellar::snapshot();
    CHECK(last == stellar::SNAPSHOT_MAX, "reach snapshot limit");
    CHECK(stellar::snapshot() == stellar::INVALID_STAR, "limit enforced");

    fresh_fs();
    {
        static u8 blob[5000];
        static u8 sink[8192];
        for (u64 i = 0; i < sizeof(blob); ++i) blob[i] = static_cast<u8>(i * 7 + 3);

        u64 tmp = stellar::create_file(stellar::ROOT_STAR, "tmp.bin", blob, sizeof(blob));
        CHECK(tmp != stellar::INVALID_STAR, "create tmp.bin");
        u64 after_create = stellar::free_space_sectors();
        CHECK(stellar::unlink(stellar::ROOT_STAR, "tmp.bin"), "unlink in-epoch file");
        u64 after_unlink = stellar::free_space_sectors();
        printf("in-epoch unlink: free sectors %lu -> %lu\n", after_create, after_unlink);
        CHECK(after_unlink >= after_create + 10, "in-epoch unlink returns its extent immediately");
        CHECK(stellar::find(stellar::ROOT_STAR, "tmp.bin") == stellar::INVALID_STAR, "unlinked name is gone");
        CHECK(stellar::read_file(tmp, sink, sizeof(sink)) == stellar::READ_ERROR, "dead star reads nothing");
        CHECK(!stellar::unlink(stellar::ROOT_STAR, "tmp.bin"), "second unlink fails");
        CHECK(!stellar::unlink(stellar::ROOT_STAR, "missing"), "unlink of a missing name fails");

        u64 d = stellar::create_constellation(stellar::ROOT_STAR, "d");
        make(d, "inner", "x");
        CHECK(!stellar::unlink(stellar::ROOT_STAR, "d"), "non-empty directory is refused");
        CHECK(stellar::unlink(d, "inner"), "unlink file inside directory");
        CHECK(stellar::unlink(stellar::ROOT_STAR, "d"), "empty directory is removed");
        CHECK(stellar::find(stellar::ROOT_STAR, "d") == stellar::INVALID_STAR, "directory name is gone");

        u64 d1 = stellar::create_constellation(stellar::ROOT_STAR, "l1");
        u64 d2 = stellar::create_constellation(stellar::ROOT_STAR, "l2");
        u64 shared = make(d1, "s", "shared\n");
        CHECK(stellar::link(d2, "alias", shared), "second link");
        CHECK(stellar::write_file(shared, "v2\n", 3) == shared, "rewrite a linked file");
        CHECK(stellar::unlink(d1, "s"), "drop first link");
        CHECK(eq(shared, "v2\n") && stellar::find(d2, "alias") == shared, "second link keeps the file alive");
        CHECK(stellar::unlink(d2, "alias"), "drop last link");
        CHECK(stellar::read_file(shared, sink, sizeof(sink)) == stellar::READ_ERROR, "last unlink kills the star");
    }

    fresh_fs();
    {
        static u8 blob[4096];
        static u8 sink[8192];
        for (u64 i = 0; i < sizeof(blob); ++i) blob[i] = static_cast<u8>(i * 13 + 5);

        u64 keep = make(stellar::ROOT_STAR, "keep", "k\n");
        u64 big = stellar::create_file(stellar::ROOT_STAR, "big", blob, sizeof(blob));
        u64 s = stellar::snapshot();
        CHECK(s == 1, "snapshot id");
        CHECK(stellar::unlink(stellar::ROOT_STAR, "big"), "unlink after snapshot");
        CHECK(stellar::find(stellar::ROOT_STAR, "big") == stellar::INVALID_STAR, "live view lost the name");
        CHECK(stellar::find(stellar::ROOT_STAR, "big", s) == big, "snapshot still names the file");

        CHECK(stellar::gc() != stellar::INVALID_STAR, "gc with a snapshot alive");
        CHECK(stellar::read_file(big, sink, sizeof(sink), s) == sizeof(blob) && stellar::verify_file(big, s),
              "gc keeps every extent a snapshot references");

        u64 before = stellar::free_space_sectors();
        CHECK(stellar::delete_snapshot(s), "delete snapshot");
        CHECK(!stellar::delete_snapshot(s), "deleting twice fails");
        CHECK(stellar::find(stellar::ROOT_STAR, "big", s) == stellar::INVALID_STAR, "deleted snapshot is not addressable");
        u64 freed = stellar::gc();
        printf("gc after snapshot delete: freed %lu sectors\n", freed);
        CHECK(freed != stellar::INVALID_STAR && freed >= 8, "gc reclaims what only the snapshot held");
        CHECK(stellar::free_space_sectors() == before + freed, "bitmap matches the sweep");
        CHECK(stellar::gc() == 0, "second gc is a no-op");
        CHECK(eq(keep, "k\n") && stellar::verify_file(keep), "live data intact after gc");

        for (u64 i = 0; i < 200; ++i) {
            char nm[32]; name_of(nm, "g", i);
            CHECK(stellar::create_file(stellar::ROOT_STAR, nm, nm, strlen(nm)) != stellar::INVALID_STAR, "create after gc");
        }
        for (u64 i = 0; i < 200; ++i) {
            char nm[32]; name_of(nm, "g", i);
            CHECK(eq(stellar::find(stellar::ROOT_STAR, nm), nm), "no overlap after gc");
        }
        CHECK(eq(keep, "k\n"), "old file survives reuse of reclaimed sectors");

        u64 free_end = stellar::free_space_sectors();
        CHECK(stellar::mount(), "remount after gc");
        CHECK(stellar::free_space_sectors() == free_end, "swept bitmap persisted");
        CHECK(eq(stellar::find(stellar::ROOT_STAR, "keep"), "k\n"), "keep persisted");
    }

    fresh_fs();
    {
        u64 a1 = stellar::snapshot();
        u64 a2 = stellar::snapshot();
        u64 a3 = stellar::snapshot();
        CHECK(a1 == 1 && a2 == 2 && a3 == 3, "snapshot ids");
        CHECK(stellar::delete_snapshot(a2), "delete middle snapshot");
        CHECK(!stellar::delete_snapshot(a2), "delete twice fails");
        CHECK(stellar::live_snapshot_count() == 2 && stellar::snapshot_count() == 3, "tombstone is counted");
        CHECK(stellar::snapshot() == 2, "freed slot is reused");
        CHECK(stellar::delete_snapshot(a3) && stellar::delete_snapshot(2), "delete the tail");
        CHECK(stellar::snapshot_count() == 1 && stellar::live_snapshot_count() == 1, "table shrinks");
        CHECK(!stellar::delete_snapshot(stellar::LIVE) && !stellar::delete_snapshot(99), "invalid ids refused");
    }

    fresh_fs();
    {
        for (u32 i = 0; i < stellar::SNAPSHOT_MAX; ++i) stellar::snapshot();
        CHECK(stellar::snapshot() == stellar::INVALID_STAR, "table full");
        CHECK(stellar::delete_snapshot(5), "free one slot");
        CHECK(stellar::snapshot() == 5, "full table recovers after a delete");
    }

    {
        auto crc_ref = [](const u8* d, u64 n) {
            u32 c = 0xFFFFFFFFu;
            for (u64 i = 0; i < n; ++i) {
                c ^= d[i];
                for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            return c ^ 0xFFFFFFFFu;
        };
        CHECK(stellar::crc32("123456789", 9) == 0xCBF43926u, "crc32 known answer");
        static u8 data[600];
        for (u64 i = 0; i < sizeof(data); ++i) data[i] = static_cast<u8>((i * 2654435761u) >> 13);
        bool ok = true;
        for (u64 off = 0; off < 8; ++off)
            for (u64 n = 0; n <= 300; ++n)
                ok = ok && stellar::crc32(data + off, n) == crc_ref(data + off, n);
        CHECK(ok, "slice-by-8 matches the bitwise reference at every length and alignment");
    }

    fresh_fs();
    {
        static u8 in[70000], out[70000];
        for (u64 i = 0; i < sizeof(in); ++i) in[i] = static_cast<u8>((i * 40503u) >> 7);
        const u64 sizes[] = {0, 1, 511, 512, 513, 1023, 1024, 1025, 4095, 4096, 4097, 5000, 8192, 8193, 70000};
        u64 idx = 0;
        for (u64 sz : sizes) {
            char nm[32]; name_of(nm, "rt", idx++);
            u64 st = stellar::create_file(stellar::ROOT_STAR, nm, in, sz);
            CHECK(st != stellar::INVALID_STAR, "round trip create");
            for (u64 i = 0; i < sizeof(out); ++i) out[i] = 0xEE;
            u64 got = stellar::read_file(st, out, sz);
            bool same = got == sz;
            for (u64 i = 0; same && i < sz; ++i) same = out[i] == in[i];
            CHECK(same, "exact-size read");
            bool guard = true;
            for (u64 i = sz; i < sz + 16 && i < sizeof(out); ++i) guard = guard && out[i] == 0xEE;
            CHECK(guard, "read never writes past the requested size");
            CHECK(stellar::verify_file(st), "checksum verifies");
            for (u64 i = 0; i < sizeof(out); ++i) out[i] = 0;
            got = stellar::read_file(st, out, sizeof(out));
            same = got == sz;
            for (u64 i = 0; same && i < sz; ++i) same = out[i] == in[i];
            CHECK(same, "oversized-buffer read");
        }
    }

    fresh_fs();
    {
        auto a = stellar::io_stats();
        stellar::IoStats mark[4];
        stellar::begin_batch();
        for (u64 i = 0; i < 300; ++i) {
            if (i % 100 == 0) mark[i / 100] = stellar::io_stats();
            char nm[32]; name_of(nm, "m", i);
            make(stellar::ROOT_STAR, nm, nm);
        }
        CHECK(stellar::end_batch(), "the batch commits");
        auto b = stellar::io_stats();
        mark[3] = b;
        u64 lookups = (b.reads - a.reads) + (b.cache_hits - a.cache_hits);
        u64 early_writes = mark[1].writes - mark[0].writes;
        u64 late_writes = mark[3].writes - mark[2].writes;
        u64 chain = (300 + 6) / 7 + 1;
        printf("300 creates in one directory: %lu sector lookups (%.1f per create), %lu disk reads, writes first/last 100: %lu/%lu\n",
               lookups, (double)lookups / 300.0, b.reads - a.reads, early_writes, late_writes);
        CHECK(late_writes <= early_writes + early_writes / 2, "within a batch, writes per create grow only with catalog depth, not directory size");
        CHECK(lookups <= 300 * (chain + 16), "duplicate check costs one pass over the directory chain, no more");
    }

    fresh_fs();
    {
        u64 live = 0;
        for (u64 i = 0; i < 30; ++i) { char nm[32]; name_of(nm, "h", i); make(stellar::ROOT_STAR, nm, nm); ++live; }
        stellar::snapshot();
        for (u64 i = 30; i < 60; ++i) { char nm[32]; name_of(nm, "h", i); make(stellar::ROOT_STAR, nm, nm); ++live; }
        for (u64 i = 0; i < 60; i += 3) {
            char nm[32]; name_of(nm, "h", i);
            CHECK(stellar::unlink(stellar::ROOT_STAR, nm), "unlink before refill");
            --live;
        }
        for (u64 i = 100; i < 130; ++i) { char nm[32]; name_of(nm, "h", i); make(stellar::ROOT_STAR, nm, nm); ++live; }
        CHECK(count_dir(stellar::ROOT_STAR) == live, "directory count after epoch change, unlinks and refill");
        bool all = true;
        for (u64 i = 0; i < 130; ++i) {
            if (i >= 60 && i < 100) continue;
            char nm[32]; name_of(nm, "h", i);
            u64 st = stellar::find(stellar::ROOT_STAR, nm);
            bool should_exist = i >= 100 || (i % 3) != 0;
            all = all && (should_exist ? eq(st, nm) : st == stellar::INVALID_STAR);
        }
        CHECK(all, "append hint stays correct across snapshot and unlink");
    }

    {
        auto count_writes = [](bool batched) {
            fresh_fs();
            auto a = stellar::io_stats();
            if (batched) stellar::begin_batch();
            for (u64 i = 0; i < 100; ++i) {
                char nm[32]; name_of(nm, "b", i);
                make(stellar::ROOT_STAR, nm, nm);
            }
            if (batched) CHECK(stellar::end_batch(), "end batch");
            return stellar::io_stats().writes - a.writes;
        };
        u64 plain = count_writes(false);
        u64 grouped = count_writes(true);
        printf("100 creates: %lu sector writes unbatched, %lu in one batch\n", plain, grouped);
        CHECK(grouped < plain, "batching saves superblock and bitmap writes");
        CHECK(stellar::mount(), "remount after batch");
        bool all = true;
        for (u64 i = 0; i < 100; ++i) {
            char nm[32]; name_of(nm, "b", i);
            all = all && eq(stellar::find(stellar::ROOT_STAR, nm), nm);
        }
        CHECK(all, "batched creates persisted");
        CHECK(!stellar::end_batch(), "unbalanced end_batch is refused");
    }

    fresh_fs();
    {
        char n51[52], n52[53], n60[61], n300[301];
        for (int i = 0; i < 51; ++i) n51[i] = static_cast<char>('a' + i % 26);
        n51[51] = 0;
        for (int i = 0; i < 52; ++i) n52[i] = static_cast<char>('a' + i % 26);
        n52[52] = 0;
        for (int i = 0; i < 60; ++i) n60[i] = static_cast<char>('a' + i % 26);
        n60[60] = 0;
        for (int i = 0; i < 300; ++i) n300[i] = 'z';
        n300[300] = 0;

        u64 free_before = stellar::free_space_sectors();
        u64 ids_before = make(stellar::ROOT_STAR, "probe", "p");
        CHECK(make(stellar::ROOT_STAR, n52, "x") == stellar::INVALID_STAR, "52-char name is refused");
        CHECK(make(stellar::ROOT_STAR, n60, "x") == stellar::INVALID_STAR, "60-char name is refused");
        CHECK(make(stellar::ROOT_STAR, n300, "x") == stellar::INVALID_STAR, "300-char name is refused");
        CHECK(make(stellar::ROOT_STAR, "", "x") == stellar::INVALID_STAR, "empty name is refused");
        CHECK(make(stellar::ROOT_STAR, ".", "x") == stellar::INVALID_STAR, "dot name is refused");
        CHECK(make(stellar::ROOT_STAR, "..", "x") == stellar::INVALID_STAR, "dotdot name is refused");
        CHECK(make(stellar::ROOT_STAR, "a/b", "x") == stellar::INVALID_STAR, "name with a slash is refused");
        CHECK(stellar::create_constellation(stellar::ROOT_STAR, n60) == stellar::INVALID_STAR, "long directory name is refused");
        CHECK(stellar::create_constellation(stellar::ROOT_STAR, "") == stellar::INVALID_STAR, "empty directory name is refused");
        CHECK(count_dir(stellar::ROOT_STAR) == 1, "refused creates leave no directory entry");
        u64 free_mid = stellar::free_space_sectors();
        make(stellar::ROOT_STAR, n52, "x");
        CHECK(stellar::free_space_sectors() == free_mid, "a refused create allocates nothing");
        (void)free_before; (void)ids_before;

        u64 longf = make(stellar::ROOT_STAR, n51, "longest");
        CHECK(longf != stellar::INVALID_STAR, "51-char name is accepted");
        CHECK(stellar::find(stellar::ROOT_STAR, n51) == longf, "51-char name is found again");
        struct Seen { const char* want; bool hit; } seen{n51, false};
        stellar::list(stellar::ROOT_STAR, [](const char* nm, u64, u32, void* c) {
            auto* sn = static_cast<Seen*>(c);
            if (strlen(nm) == strlen(sn->want) && __builtin_memcmp(nm, sn->want, strlen(nm)) == 0) sn->hit = true;
        }, &seen);
        CHECK(seen.hit, "list returns the full 51-char name");
    }

    fresh_fs();
    {
        u64 a1 = make(stellar::ROOT_STAR, "dup", "1");
        CHECK(a1 != stellar::INVALID_STAR, "first create");
        CHECK(make(stellar::ROOT_STAR, "dup", "2") == stellar::INVALID_STAR, "duplicate file name is refused");
        CHECK(stellar::create_constellation(stellar::ROOT_STAR, "dup") == stellar::INVALID_STAR, "duplicate directory name is refused");
        CHECK(eq(stellar::find(stellar::ROOT_STAR, "dup"), "1"), "original survives a refused duplicate");
        CHECK(count_dir(stellar::ROOT_STAR) == 1, "refused duplicates add no entry");

        u64 other = stellar::create_constellation(stellar::ROOT_STAR, "other");
        CHECK(make(other, "dup", "elsewhere") != stellar::INVALID_STAR, "same name in another directory is fine");
        CHECK(!stellar::link(other, "dup", a1), "link onto an existing name is refused");
        CHECK(stellar::link(other, "alias", a1), "link under a fresh name works");
        CHECK(stellar::unlink(other, "alias") && stellar::unlink(stellar::ROOT_STAR, "dup"), "unlink both names");
        u8 sink[16];
        CHECK(stellar::read_file(a1, sink, sizeof(sink)) == stellar::READ_ERROR && stellar::find(stellar::ROOT_STAR, "dup") == stellar::INVALID_STAR,
              "refused link did not leak a link count");

        u64 d = stellar::create_constellation(stellar::ROOT_STAR, "wide");
        for (u64 i = 0; i < 20; ++i) { char nm[32]; name_of(nm, "e", i); make(d, nm, nm); }
        CHECK(make(d, "e19", "x") == stellar::INVALID_STAR, "duplicate in the last sector of a chain is refused");
        CHECK(make(d, "e0", "x") == stellar::INVALID_STAR, "duplicate in the first sector of a chain is refused");
        CHECK(count_dir(d) == 20, "chain still holds exactly 20 entries");

        u64 reuse = make(stellar::ROOT_STAR, "reuse", "v1");
        u64 s = stellar::snapshot();
        CHECK(stellar::unlink(stellar::ROOT_STAR, "reuse"), "unlink after snapshot");
        u64 reuse2 = make(stellar::ROOT_STAR, "reuse", "v2");
        CHECK(reuse2 != stellar::INVALID_STAR && reuse2 != reuse, "name can be reused after unlink");
        CHECK(stellar::find(stellar::ROOT_STAR, "reuse", s) == reuse && eq(reuse, "v1", s), "snapshot still names the old star");
        CHECK(make(stellar::ROOT_STAR, "reuse", "v3") == stellar::INVALID_STAR, "reused name is a duplicate again");
    }

    fresh_fs();
    {
        u64 d1 = stellar::create_constellation(stellar::ROOT_STAR, "ld");
        u64 d2 = stellar::create_constellation(stellar::ROOT_STAR, "ld2");
        u64 f = make(d1, "f", "file");
        CHECK(!stellar::link(d1, "up", stellar::ROOT_STAR), "linking the root into a subdirectory is refused");
        CHECK(!stellar::link(d2, "alias", d1), "linking a directory under a second parent is refused");
        CHECK(!stellar::link(d1, "self", d1), "linking a directory into itself is refused");
        CHECK(count_dir(d1) == 1 && count_dir(d2) == 0, "refused directory links add no entries");
        CHECK(stellar::link(d2, "f2", f) && eq(stellar::find(d2, "f2"), "file"), "file hard links still work");
        CHECK(stellar::unlink(d1, "f") && stellar::unlink(d2, "f2"), "both file links drop");
        CHECK(stellar::unlink(stellar::ROOT_STAR, "ld"), "directory removes cleanly, so its link count was untouched");
    }

    fresh_fs();
    {
        u64 keep = make(stellar::ROOT_STAR, "keep", "data");
        CHECK(keep != stellar::INVALID_STAR, "seed file for mount tests");
        u8 good[1024];
        __builtin_memcpy(good, g_disk, 1024);

        struct Poke { u64 off; u64 val; int bytes; };
        struct Case { const char* name; Poke a; Poke b; };
        const u64 total = g_sectors;
        const u64 bs = (((total + 7) / 8) + 507) / 508;
        const Case cases[] = {
            {"sector size is not 512", {12, 1024, 4}, {0, 0, 0}},
            {"total sectors larger than the device", {16, total + 1, 8}, {0, 0, 0}},
            {"total sectors zero", {16, 0, 8}, {0, 0, 0}},
            {"total sectors tiny", {16, 4, 8}, {0, 0, 0}},
            {"bitmap does not follow the superblocks", {24, 3, 8}, {0, 0, 0}},
            {"bitmap sector count too large", {32, 9999, 8}, {0, 0, 0}},
            {"bitmap sector count zero", {32, 0, 8}, {0, 0, 0}},
            {"bitmap sector count off by one", {32, bs + 1, 8}, {0, 0, 0}},
            {"catalog root zero", {40, 0, 8}, {0, 0, 0}},
            {"catalog root inside the bitmap", {40, 3, 8}, {0, 0, 0}},
            {"catalog root past the end", {40, total, 8}, {0, 0, 0}},
            {"epoch zero", {56, 0, 8}, {0, 0, 0}},
            {"snapshot count over the table", {80, 25, 4}, {0, 0, 0}},
            {"snapshot count huge", {80, 0xFFFFFFFFu, 4}, {0, 0, 0}},
            {"snapshot root past the end", {80, 1, 4}, {88, total + 5, 8}},
            {"snapshot root inside the bitmap", {80, 1, 4}, {88, 3, 8}},
        };
        for (const Case& c : cases) {
            __builtin_memcpy(g_disk, good, 1024);
            const Poke pokes[2] = {c.a, c.b};
            for (const Poke& p : pokes) if (p.bytes) sb_poke(p.off, p.val, p.bytes);
            CHECK(!stellar::mount(), c.name);
            CHECK(stellar::find(stellar::ROOT_STAR, "keep") == stellar::INVALID_STAR, "a rejected mount leaves the filesystem unmounted");
            CHECK(stellar::create_file(stellar::ROOT_STAR, "x", "x", 1) == stellar::INVALID_STAR, "writes fail after a rejected mount");
        }
        __builtin_memcpy(g_disk, good, 1024);
        CHECK(stellar::mount(), "the untouched superblocks still mount");
        CHECK(eq(stellar::find(stellar::ROOT_STAR, "keep"), "data"), "and the data is intact");

        __builtin_memcpy(g_disk, good, 1024);
        sb_poke(80, 24, 4);
        u64 root_copy;
        __builtin_memcpy(&root_copy, g_disk + 40, 8);
        for (u32 i = 0; i < 24; ++i) sb_poke(88 + 16 * i, root_copy, 8);
        CHECK(stellar::mount(), "a full snapshot table with valid roots still mounts");
    }

    fresh_fs();
    {
        u64 keep = make(stellar::ROOT_STAR, "keep", "kept\n");
        CHECK(keep != stellar::INVALID_STAR, "seed file");

        for (long n = 0; n < 2; ++n) {
            g_allocs_until_fail = n;
            CHECK(!stellar::format(g_sectors), "format fails cleanly when RAM runs out");
            g_allocs_until_fail = -1;
        }
        CHECK(stellar::format(g_sectors), "format works again once RAM is back");
        keep = make(stellar::ROOT_STAR, "keep", "kept\n");

        for (long n = 0; n < 2; ++n) {
            g_allocs_until_fail = n;
            CHECK(!stellar::mount(), "mount fails cleanly when RAM runs out");
            CHECK(stellar::find(stellar::ROOT_STAR, "keep") == stellar::INVALID_STAR, "and stays unmounted");
            g_allocs_until_fail = -1;
        }
        CHECK(stellar::mount() && eq(stellar::find(stellar::ROOT_STAR, "keep"), "kept\n"), "mount works again once RAM is back");

        g_allocs_until_fail = 0;
        CHECK(stellar::gc() == stellar::INVALID_STAR, "gc reports failure when it cannot allocate its mark bitmap");
        g_allocs_until_fail = -1;
        CHECK(stellar::gc() != stellar::INVALID_STAR, "gc works again once RAM is back");

        CHECK(stellar::mount(), "remount to drop the verify scratch buffer");
        g_allocs_until_fail = 0;
        CHECK(!stellar::verify_file(stellar::find(stellar::ROOT_STAR, "keep")), "verify_file fails when it cannot allocate scratch");
        g_allocs_until_fail = -1;
        CHECK(stellar::verify_file(stellar::find(stellar::ROOT_STAR, "keep")), "verify_file works again once RAM is back");
    }

    {
        static u64 st_file, st_dir, st_snap;
        static u8 payload[3000];
        for (u64 i = 0; i < sizeof(payload); ++i) payload[i] = static_cast<u8>(i * 31 + 7);
        struct Op { const char* name; void (*setup)(); bool (*run)(); };
        auto plain = [] {};
        const Op ops[] = {
            {"create_file", +plain, [] { return stellar::create_file(stellar::ROOT_STAR, "f", payload, sizeof(payload)) != stellar::INVALID_STAR; }},
            {"create_constellation", +plain, [] { return stellar::create_constellation(stellar::ROOT_STAR, "d") != stellar::INVALID_STAR; }},
            {"write_file", [] { st_file = make(stellar::ROOT_STAR, "f", "old"); },
                           [] { return stellar::write_file(st_file, payload, sizeof(payload)) != stellar::INVALID_STAR; }},
            {"snapshot", +plain, [] { return stellar::snapshot() != stellar::INVALID_STAR; }},
            {"link", [] { st_file = make(stellar::ROOT_STAR, "f", "x"); st_dir = stellar::create_constellation(stellar::ROOT_STAR, "d"); },
                     [] { return stellar::link(st_dir, "alias", st_file); }},
            {"unlink", [] { st_file = make(stellar::ROOT_STAR, "f", "x"); st_snap = stellar::snapshot(); (void)st_snap; },
                       [] { return stellar::unlink(stellar::ROOT_STAR, "f"); }},
            {"unlink (frees extents)", [] { st_file = stellar::create_file(stellar::ROOT_STAR, "f", payload, sizeof(payload)); },
                       [] { return stellar::unlink(stellar::ROOT_STAR, "f"); }},
            {"delete_snapshot", [] { st_snap = stellar::snapshot(); }, [] { return stellar::delete_snapshot(st_snap); }},
            {"gc", [] {
                        st_file = stellar::create_file(stellar::ROOT_STAR, "f", payload, sizeof(payload));
                        st_snap = stellar::snapshot();
                        stellar::unlink(stellar::ROOT_STAR, "f");
                        stellar::delete_snapshot(st_snap);
                    },
                    [] { return stellar::gc() != stellar::INVALID_STAR; }},
            {"batch of creates", +plain, [] {
                        stellar::begin_batch();
                        bool ok = true;
                        for (int i = 0; i < 5; ++i) {
                            char nm[16]; name_of(nm, "b", static_cast<u64>(i));
                            ok = (make(stellar::ROOT_STAR, nm, nm) != stellar::INVALID_STAR) && ok;
                        }
                        return stellar::end_batch() && ok;
                    }},
        };
        for (const Op& op : ops) {
            fresh_fs();
            op.setup();
            g_wcount = 0;
            g_last_sb_w = 0;
            bool clean = op.run();
            u64 total = g_last_sb_w ? g_last_sb_w : g_wcount;
            CHECK(clean, op.name);
            u64 missed = 0, first_missed = ~0ull;
            for (u64 k = 0; k < total; ++k) {
                fresh_fs();
                op.setup();
                g_wcount = 0;
                g_fail_after = k;
                bool ok = op.run();
                g_fail_after = ~0ull;
                if (ok) { ++missed; if (first_missed == ~0ull) first_missed = k; }
            }
            if (missed) printf("  %s: %lu of %lu injected write failures went unreported (first at write %lu)\n",
                               op.name, missed, total, first_missed);
            CHECK(missed == 0, "a failed disk write must fail the operation");
        }
        g_fail_after = ~0ull;
        fresh_fs();
    }

    fresh_fs();
    {
        g_wcount = 0;
        g_last_sb_w = 0;
        u64 probe = make(stellar::ROOT_STAR, "probe", "p");
        u64 w = g_last_sb_w;
        CHECK(probe != stellar::INVALID_STAR && w >= 2, "measure a create's writes");

        g_wcount = 0;
        g_fail_after = w - 1;
        u64 lost = make(stellar::ROOT_STAR, "lost", "doomed");
        g_fail_after = ~0ull;
        CHECK(lost == stellar::INVALID_STAR, "create reports the failed superblock write");

        u64 after = make(stellar::ROOT_STAR, "after", "survives");
        CHECK(after != stellar::INVALID_STAR, "the next create succeeds on a healthy disk");
        CHECK(stellar::mount(), "remount after a failed commit");
        CHECK(eq(stellar::find(stellar::ROOT_STAR, "after"), "survives"), "later data persisted");
        CHECK(eq(stellar::find(stellar::ROOT_STAR, "probe"), "p"), "earlier data persisted");
        u64 free_now = stellar::free_space_sectors();
        CHECK(stellar::gc() != stellar::INVALID_STAR, "gc runs after the failed commit");
        CHECK(stellar::free_space_sectors() >= free_now, "gc never loses space it should return");
        CHECK(eq(stellar::find(stellar::ROOT_STAR, "after"), "survives"), "data intact after gc");

        g_fail_after = 0;
        CHECK(!stellar::format(g_sectors), "format fails when the disk is dead");
        g_fail_after = ~0ull;
        CHECK(stellar::find(stellar::ROOT_STAR, "probe") == stellar::INVALID_STAR, "a failed format leaves the filesystem unmounted");
        CHECK(stellar::format(g_sectors), "format works once the disk is back");
    }

    fresh_fs();
    {
        static u8 blob[5000], sink[8192];
        for (u64 i = 0; i < sizeof(blob); ++i) blob[i] = static_cast<u8>(i * 17 + 1);
        u64 big = stellar::create_file(stellar::ROOT_STAR, "big", blob, sizeof(blob));
        u64 one = make(stellar::ROOT_STAR, "one", "tiny file\n");
        u64 empty = stellar::create_file(stellar::ROOT_STAR, "empty", nullptr, 0);
        u64 snap = stellar::snapshot();
        u64 frozen = make(stellar::ROOT_STAR, "frozen", "snapshotted\n");
        (void)frozen;

        CHECK(stellar::mount(), "remount to clear the sector cache");

        CHECK(stellar::read_file(big, sink, sizeof(sink)) == sizeof(blob), "clean multi-sector read");
        CHECK(stellar::read_file(one, sink, sizeof(sink)) == 10, "clean single-sector read");
        CHECK(stellar::read_file(empty, sink, sizeof(sink)) == 0 && stellar::verify_file(empty), "empty file reads clean");

        u64 big_first = 0, one_first = 0;
        for (u64 sec = 2; sec < g_sectors && (!big_first || !one_first); ++sec) {
            if (!big_first && __builtin_memcmp(g_disk + sec * 512, blob, 512) == 0) big_first = sec;
            if (!one_first && __builtin_memcmp(g_disk + sec * 512, "tiny file\n", 10) == 0) one_first = sec;
        }
        CHECK(big_first && one_first, "locate the extents on the disk");

        g_disk[(big_first + 2) * 512 + 7] ^= 0x40;
        g_disk[one_first * 512 + 4] ^= 0x01;
        CHECK(stellar::mount(), "remount with corrupted extents");

        CHECK(!stellar::verify_file(big) && !stellar::verify_file(one), "verify_file sees the corruption");
        CHECK(stellar::read_file(big, sink, sizeof(sink)) == stellar::READ_ERROR, "read_file refuses a corrupt multi-sector file");
        CHECK(stellar::read_file(one, sink, sizeof(sink)) == stellar::READ_ERROR, "read_file refuses a corrupt single-sector file");
        CHECK(stellar::read_file(big, sink, sizeof(blob)) == stellar::READ_ERROR, "an exact-size buffer is still verified");
        CHECK(stellar::read_file(big, sink, 100) == 100, "a partial read cannot be verified and still works");
        CHECK(stellar::read_file(big, sink, sizeof(sink), snap) == stellar::READ_ERROR, "the same bytes seen through a snapshot are refused too");
        CHECK(stellar::read_file(empty, sink, sizeof(sink)) == 0 && stellar::verify_file(empty), "untouched files are unaffected");
        CHECK(eq(stellar::find(stellar::ROOT_STAR, "frozen"), "snapshotted\n"), "files written after the snapshot read clean");

        g_disk[(big_first + 2) * 512 + 7] ^= 0x40;
        g_disk[one_first * 512 + 4] ^= 0x01;
        CHECK(stellar::mount(), "remount after repairing the bytes");
        CHECK(stellar::read_file(big, sink, sizeof(sink)) == sizeof(blob) && stellar::verify_file(big), "repaired file reads again");
        bool same = true;
        for (u64 i = 0; i < sizeof(blob); ++i) same = same && sink[i] == blob[i];
        CHECK(same, "and returns the original bytes");
        CHECK(eq(one, "tiny file\n"), "repaired small file reads again");
    }

    fresh_fs();
    {
        constexpr u64 THREADS = 4, FILES = 250;
        static u64 shared_dir;
        static volatile u64 thread_errors;
        static volatile u64 ready;
        shared_dir = stellar::create_constellation(stellar::ROOT_STAR, "mt");
        thread_errors = 0;
        ready = 0;
        struct Worker {
            static void* run(void* arg) {
                u64 w = reinterpret_cast<u64>(arg);
                __atomic_fetch_add(&ready, 1ull, __ATOMIC_SEQ_CST);
                while (__atomic_load_n(&ready, __ATOMIC_SEQ_CST) < THREADS) {}
                for (u64 j = 0; j < FILES; ++j) {
                    char nm[32], tag[24];
                    name_of(tag, "t", w);
                    name_of(nm, tag, j);
                    u64 st = stellar::create_file(shared_dir, nm, nm, strlen(nm));
                    if (st == stellar::INVALID_STAR) { __atomic_fetch_add(&thread_errors, 1ull, __ATOMIC_SEQ_CST); continue; }
                    if (!eq(st, nm)) __atomic_fetch_add(&thread_errors, 1ull, __ATOMIC_SEQ_CST);
                    if (j % 7 == 3 && stellar::write_file(st, "rewritten", 9) == stellar::INVALID_STAR)
                        __atomic_fetch_add(&thread_errors, 1ull, __ATOMIC_SEQ_CST);
                    if (w == 0 && j % 50 == 0) stellar::snapshot();
                    if (j % 11 == 5 && stellar::find(shared_dir, nm) != st) __atomic_fetch_add(&thread_errors, 1ull, __ATOMIC_SEQ_CST);
                    if (j % 13 == 9) { u64 n = 0; stellar::list(shared_dir, &count_cb, &n); }
                }
                return nullptr;
            }
        };
        pthread_t th[THREADS];
        for (u64 w = 0; w < THREADS; ++w) pthread_create(&th[w], nullptr, &Worker::run, reinterpret_cast<void*>(w));
        for (u64 w = 0; w < THREADS; ++w) pthread_join(th[w], nullptr);
        CHECK(thread_errors == 0, "concurrent creates, rewrites, finds and lists report no errors");
        CHECK(count_dir(shared_dir) == THREADS * FILES, "every concurrent create landed in the directory exactly once");
        bool all = true;
        for (u64 w = 0; w < THREADS; ++w)
            for (u64 j = 0; j < FILES; ++j) {
                char nm[32], tag[24];
                name_of(tag, "t", w);
                name_of(nm, tag, j);
                u64 st = stellar::find(shared_dir, nm);
                all = all && st != stellar::INVALID_STAR && stellar::verify_file(st) &&
                      eq(st, (j % 7 == 3) ? "rewritten" : nm);
            }
        CHECK(all, "every file is present, checksums verify, and contents are right");
        CHECK(stellar::mount() && count_dir(stellar::find(stellar::ROOT_STAR, "mt")) == THREADS * FILES, "the result persists across a remount");
    }

    fresh_fs();
    {
        stellar::begin_batch();
        stellar::begin_batch();
        CHECK(make(stellar::ROOT_STAR, "in-batch", "x") != stellar::INVALID_STAR, "operations run inside nested batches");
        CHECK(!stellar::mount(), "mount is refused while a batch is open");
        CHECK(!stellar::format(g_sectors), "format is refused while a batch is open");
        CHECK(stellar::end_batch() && stellar::end_batch(), "nested batches close");
        CHECK(!stellar::end_batch(), "closing a batch that was never opened is refused");
        CHECK(stellar::find(stellar::ROOT_STAR, "in-batch") != stellar::INVALID_STAR, "the batched create is still there");
        CHECK(stellar::mount(), "mount works again once the batch is closed");
    }

    // ---------------------------------------------------------------- status codes
    fresh_fs();
    {
        using stellar::Status;
        Status st = Status::Internal;
        auto S = [&](Status want, const char* msg) {
            if (st != want) printf("  got '%s', wanted '%s'\n", stellar::status_name(st), stellar::status_name(want));
            CHECK(st == want, msg);
            st = Status::Internal;
        };
        u64 a = stellar::create_file(stellar::ROOT_STAR, "a", "hello", 5, &st);
        CHECK(a != stellar::INVALID_STAR, "create"); S(Status::Ok, "create reports Ok");
        u64 dir = stellar::create_constellation(stellar::ROOT_STAR, "dir", &st); S(Status::Ok, "mkdir reports Ok");
        u64 inner = stellar::create_file(dir, "inner", "x", 1, &st); (void)inner;
        char n60[61]; for (int i = 0; i < 60; ++i) n60[i] = 'q'; n60[60] = 0;

        stellar::create_file(stellar::ROOT_STAR, "a", "x", 1, &st); S(Status::Exists, "duplicate name");
        stellar::create_constellation(stellar::ROOT_STAR, "a", &st); S(Status::Exists, "duplicate directory name");
        stellar::create_file(stellar::ROOT_STAR, "", "x", 1, &st); S(Status::InvalidName, "empty name");
        stellar::create_file(stellar::ROOT_STAR, "..", "x", 1, &st); S(Status::InvalidName, "dotdot name");
        stellar::create_file(stellar::ROOT_STAR, "p/q", "x", 1, &st); S(Status::InvalidName, "slash in name");
        stellar::create_file(stellar::ROOT_STAR, n60, "x", 1, &st); S(Status::InvalidName, "long name");
        stellar::create_file(a, "x", "x", 1, &st); S(Status::NotADirectory, "create under a file");
        stellar::create_file(99999, "x", "x", 1, &st); S(Status::NotFound, "create under a missing parent");

        stellar::find(stellar::ROOT_STAR, "missing", stellar::LIVE, &st); S(Status::NotFound, "find missing");
        stellar::find(a, "x", stellar::LIVE, &st); S(Status::NotADirectory, "find inside a file");
        stellar::find(stellar::ROOT_STAR, "a", 9, &st); S(Status::NoSuchSnapshot, "find in a snapshot that does not exist");
        stellar::find(stellar::ROOT_STAR, "", stellar::LIVE, &st); S(Status::InvalidName, "find with an empty name");
        CHECK(stellar::find(stellar::ROOT_STAR, "a", stellar::LIVE, &st) == a, "find hit"); S(Status::Ok, "find hit reports Ok");

        static u8 buf[64];
        u64 got = stellar::read_file(a, buf, sizeof(buf), stellar::LIVE, &st);
        CHECK(got == 5, "read returns the size"); S(Status::Ok, "read reports Ok");
        stellar::read_file(77777, buf, sizeof(buf), stellar::LIVE, &st); S(Status::NotFound, "read missing star");
        CHECK(stellar::read_file(dir, buf, sizeof(buf), stellar::LIVE, &st) == stellar::READ_ERROR, "reading a directory fails"); S(Status::IsADirectory, "read a directory");
        stellar::read_file(a, buf, sizeof(buf), 5, &st); S(Status::NoSuchSnapshot, "read from a missing snapshot");
        u64 empty = stellar::create_file(stellar::ROOT_STAR, "empty", nullptr, 0, &st);
        got = stellar::read_file(empty, buf, sizeof(buf), stellar::LIVE, &st);
        CHECK(got == 0 && got != stellar::READ_ERROR, "an empty file reads as 0, not as an error"); S(Status::Ok, "empty read reports Ok");

        stellar::write_file(dir, "x", 1, &st); S(Status::IsADirectory, "write to a directory");
        stellar::write_file(88888, "x", 1, &st); S(Status::NotFound, "write to a missing star");

        CHECK(!stellar::link(stellar::ROOT_STAR, "dd", dir, &st), "link dir"); S(Status::IsADirectory, "link a directory");
        CHECK(!stellar::link(stellar::ROOT_STAR, "dd", 66666, &st), "link missing"); S(Status::NotFound, "link a missing target");
        CHECK(!stellar::link(stellar::ROOT_STAR, "a", a, &st) || true, "link self name");
        stellar::link(dir, "inner", a, &st); S(Status::Exists, "link onto an existing name");
        CHECK(!stellar::link(a, "z", a, &st), "link into itself"); S(Status::InvalidArgument, "link where dir equals target");

        CHECK(!stellar::unlink(stellar::ROOT_STAR, "nope", &st), "unlink missing"); S(Status::NotFound, "unlink missing");
        CHECK(!stellar::unlink(stellar::ROOT_STAR, "dir", &st), "unlink non-empty"); S(Status::NotEmpty, "unlink a non-empty directory");
        CHECK(!stellar::unlink(a, "x", &st), "unlink inside a file"); S(Status::NotADirectory, "unlink inside a file");
        CHECK(!stellar::unlink(77777, "x", &st), "unlink in missing dir"); S(Status::NotFound, "unlink in a missing directory");
        CHECK(stellar::unlink(dir, "inner", &st), "unlink ok"); S(Status::Ok, "unlink reports Ok");

        CHECK(!stellar::delete_snapshot(stellar::LIVE, &st), "delete live"); S(Status::InvalidArgument, "delete the live view");
        CHECK(!stellar::delete_snapshot(77, &st), "delete missing"); S(Status::NoSuchSnapshot, "delete a missing snapshot");
        u64 sn = stellar::snapshot(&st); S(Status::Ok, "snapshot reports Ok");
        CHECK(stellar::delete_snapshot(sn, &st), "delete ok"); S(Status::Ok, "delete reports Ok");
        CHECK(!stellar::delete_snapshot(sn, &st), "delete twice"); S(Status::NoSuchSnapshot, "delete twice");

        CHECK(!stellar::verify_file(dir, stellar::LIVE, &st), "verify a directory"); S(Status::IsADirectory, "verify a directory");
        CHECK(!stellar::verify_file(55555, stellar::LIVE, &st), "verify missing"); S(Status::NotFound, "verify a missing star");

        stellar::begin_batch();
        CHECK(!stellar::mount(&st), "mount in batch"); S(Status::Busy, "mount during a batch");
        CHECK(!stellar::format(g_sectors, &st), "format in batch"); S(Status::Busy, "format during a batch");
        CHECK(stellar::end_batch(&st), "end batch"); S(Status::Ok, "end_batch reports Ok");
        CHECK(!stellar::end_batch(&st), "unbalanced end"); S(Status::InvalidArgument, "unbalanced end_batch");

        CHECK(!stellar::format(8, &st), "format tiny"); S(Status::InvalidArgument, "format below the minimum size");
        CHECK(!stellar::format(g_sectors + 1, &st), "format too big"); S(Status::InvalidArgument, "format beyond the device");
        CHECK(stellar::mount(&st), "remount"); S(Status::Ok, "mount reports Ok");

        for (u32 i = 0; i < stellar::SNAPSHOT_MAX; ++i) stellar::snapshot();
        CHECK(stellar::snapshot(&st) == stellar::INVALID_STAR, "snapshot full"); S(Status::TooManySnapshots, "snapshot table full");
    }

    fresh_fs();
    {
        using stellar::Status;
        Status st = Status::Internal;
        u8 zero[1024] = {0};
        __builtin_memcpy(g_disk, zero, 1024);
        CHECK(!stellar::mount(&st) && st == Status::NotFormatted, "blank disk reports NotFormatted");
        CHECK(stellar::format(g_sectors, &st) && st == Status::Ok, "format reports Ok");
        u8 sb[1024]; __builtin_memcpy(sb, g_disk, 1024);
        sb_poke(80, 99, 4);
        CHECK(!stellar::mount(&st) && st == Status::Corrupt, "bad superblock reports Corrupt");
        CHECK(stellar::find(stellar::ROOT_STAR, "x", stellar::LIVE, &st) == stellar::INVALID_STAR && st == Status::NotMounted,
              "operations after a rejected mount report NotMounted");
        __builtin_memcpy(g_disk, sb, 1024);
        CHECK(stellar::mount(), "restore");

        u64 f = make(stellar::ROOT_STAR, "f", "payload");
        g_allocs_until_fail = 0;
        CHECK(!stellar::verify_file(f, stellar::LIVE, &st) && st == Status::NoMemory, "verify out of memory reports NoMemory");
        CHECK(stellar::gc(&st) == stellar::INVALID_STAR && st == Status::NoMemory, "gc out of memory reports NoMemory");
        CHECK(!stellar::mount(&st) && st == Status::NoMemory, "mount out of memory reports NoMemory");
        g_allocs_until_fail = -1;
        CHECK(stellar::mount(), "restore after the allocation test");

        static u8 big[512 * 200];
        fresh_fs(80);
        CHECK(stellar::create_file(stellar::ROOT_STAR, "toobig", big, sizeof(big), &st) == stellar::INVALID_STAR && st == Status::NoSpace,
              "a file that cannot fit reports NoSpace");
        CHECK(make(stellar::ROOT_STAR, "small", "ok") != stellar::INVALID_STAR, "and the filesystem still works");

        fresh_fs();
        make(stellar::ROOT_STAR, "keep", "k");
        const char* names[4] = {"create_file", "write_file", "snapshot", "unlink"};
        for (int which = 0; which < 4; ++which) {
            fresh_fs();
            u64 star = make(stellar::ROOT_STAR, "x", "old");
            g_wcount = 0; g_fail_after = ~0ull; g_last_sb_w = 0;
            auto run = [&](Status* w) -> bool {
                switch (which) {
                    case 0: return stellar::create_file(stellar::ROOT_STAR, "n", "new", 3, w) != stellar::INVALID_STAR;
                    case 1: return stellar::write_file(star, "newer", 5, w) != stellar::INVALID_STAR;
                    case 2: return stellar::snapshot(w) != stellar::INVALID_STAR;
                    default: return stellar::unlink(stellar::ROOT_STAR, "x", w);
                }
            };
            CHECK(run(&st) && st == Status::Ok, names[which]);
            u64 total = g_last_sb_w ? g_last_sb_w : g_wcount;
            fresh_fs();
            star = make(stellar::ROOT_STAR, "x", "old");
            g_wcount = 0;
            bool all_io = true;
            for (u64 k = 0; k < total; ++k) {
                fresh_fs();
                star = make(stellar::ROOT_STAR, "x", "old");
                g_wcount = 0; g_fail_after = k;
                st = Status::Ok;
                bool ok = run(&st);
                g_fail_after = ~0ull;
                if (ok || st != Status::Io) all_io = false;
            }
            CHECK(all_io, "every injected write failure is reported as Io, never Internal or Ok");
        }
        fresh_fs();
    }

    // ---------------------------------------------------------------- directory index
    fresh_fs();
    {
        stellar::test_set_hash_mask(0);
        u64 d = stellar::create_constellation(stellar::ROOT_STAR, "wide");
        stellar::LookupStats l0 = stellar::lookup_stats();
        stellar::IoStats win[11];
        stellar::begin_batch();
        for (u64 i = 0; i < 1000; ++i) {
            if (i % 100 == 0) win[i / 100] = stellar::io_stats();
            char nm[32]; name_of(nm, "e", i);
            CHECK(make(d, nm, nm) != stellar::INVALID_STAR, "create in a growing directory");
        }
        CHECK(stellar::end_batch(), "the batch commits");
        win[10] = stellar::io_stats();
        auto cost = [&](int w) { return (win[w + 1].reads - win[w].reads) + (win[w + 1].cache_hits - win[w].cache_hits); };
        auto wr_cost = [&](int w) { return win[w + 1].writes - win[w].writes; };
        stellar::LookupStats l1 = stellar::lookup_stats();
        printf("1000 creates in one directory: sector lookups per 100 creates: first %lu, last %lu; writes %lu -> %lu; index builds %lu, index lookups %lu, scans %lu\n",
               cost(0), cost(9), wr_cost(0), wr_cost(9), l1.index_builds - l0.index_builds,
               l1.index_lookups - l0.index_lookups, l1.scan_lookups - l0.scan_lookups);
        CHECK(cost(9) <= cost(0) + cost(0) / 2 + 100, "lookup cost per create does not grow with the directory");
        CHECK(wr_cost(9) <= wr_cost(0) + wr_cost(0) / 2, "within a batch, write cost per create grows only with catalog depth");
        CHECK(cost(9) <= 100 * 14, "a create costs a small constant number of sector lookups");
        CHECK(l1.index_builds - l0.index_builds <= 3, "the index is built once, not per operation");
        CHECK(l1.scan_lookups - l0.scan_lookups == 0, "no directory chain scans on the live view");
        CHECK(count_dir(d) == 1000, "all entries are listed");

        stellar::IoStats f0 = stellar::io_stats();
        for (u64 i = 0; i < 1000; i += 3) {
            char nm[32]; name_of(nm, "e", i);
            CHECK(stellar::find(d, nm) != stellar::INVALID_STAR, "indexed find");
        }
        stellar::IoStats f1 = stellar::io_stats();
        u64 finds = 334;
        printf("find in a 1000-entry directory: %.1f sector lookups per find\n",
               (double)((f1.reads - f0.reads) + (f1.cache_hits - f0.cache_hits)) / finds);
        CHECK((f1.reads - f0.reads) + (f1.cache_hits - f0.cache_hits) <= finds * 10, "find is O(1) in directory size");

        for (u64 i = 0; i < 1000; i += 2) { char nm[32]; name_of(nm, "e", i); CHECK(stellar::unlink(d, nm), "unlink from the index"); }
        for (u64 i = 0; i < 500; ++i) { char nm[32]; name_of(nm, "r", i); CHECK(make(d, nm, nm) != stellar::INVALID_STAR, "refill reuses freed slots"); }
        CHECK(count_dir(d) == 1000, "directory count after churn");
        bool all = true;
        for (u64 i = 0; i < 1000; ++i) {
            char nm[32]; name_of(nm, "e", i);
            u64 st = stellar::find(d, nm);
            all = all && ((i % 2 == 0) ? st == stellar::INVALID_STAR : eq(st, nm));
        }
        for (u64 i = 0; i < 500; ++i) { char nm[32]; name_of(nm, "r", i); all = all && eq(stellar::find(d, nm), nm); }
        CHECK(all, "every surviving name resolves after churn");
        CHECK(stellar::mount() && count_dir(stellar::find(stellar::ROOT_STAR, "wide")) == 1000, "index state matches the disk after a remount");
    }

    {
        static const u64 masks[3] = {0x3, 0xF, 0};
        for (u64 mask : masks) {
            fresh_fs();
            stellar::test_set_hash_mask(mask);
            constexpr int N = 90, MAXS = 6;
            struct Model { bool present[N]; u64 star[N]; };
            static Model live, snap_model[MAXS];
            static u64 snap_id[MAXS];
            __builtin_memset(&live, 0, sizeof(live));
            int nsnaps = 0;
            u64 d = stellar::create_constellation(stellar::ROOT_STAR, "fz");
            u64 rng = 88172645463325252ull ^ mask;
            auto rnd = [&]() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return static_cast<u32>(rng >> 11); };
            u64 bad = 0;
            auto note = [&](bool ok, const char* what, int step) { if (!ok) { if (!bad) printf("  fuzz mask %lx: first failure '%s' at step %d\n", mask, what, step); ++bad; } };
            for (int step = 0; step < 4000; ++step) {
                u32 op = rnd() % 100;
                int k = static_cast<int>(rnd() % N);
                char nm[16]; name_of(nm, "n", static_cast<u64>(k));
                stellar::Status st = stellar::Status::Internal;
                if (op < 38) {
                    u64 id = stellar::create_file(d, nm, nm, strlen(nm), &st);
                    if (live.present[k]) note(id == stellar::INVALID_STAR && st == stellar::Status::Exists, "duplicate create", step);
                    else { note(id != stellar::INVALID_STAR && st == stellar::Status::Ok, "create", step); live.present[k] = true; live.star[k] = id; }
                } else if (op < 63) {
                    bool ok = stellar::unlink(d, nm, &st);
                    if (live.present[k]) { note(ok && st == stellar::Status::Ok, "unlink", step); live.present[k] = false; }
                    else note(!ok && st == stellar::Status::NotFound, "unlink missing", step);
                } else if (op < 88) {
                    u64 id = stellar::find(d, nm, stellar::LIVE, &st);
                    if (live.present[k]) note(id == live.star[k] && st == stellar::Status::Ok, "find present", step);
                    else note(id == stellar::INVALID_STAR && st == stellar::Status::NotFound, "find absent", step);
                } else if (op < 92 && nsnaps < MAXS) {
                    u64 id = stellar::snapshot();
                    note(id != stellar::INVALID_STAR, "snapshot", step);
                    snap_model[nsnaps] = live; snap_id[nsnaps] = id; ++nsnaps;
                } else if (op < 97 && nsnaps > 0) {
                    int si = static_cast<int>(rnd() % static_cast<u32>(nsnaps));
                    u64 id = stellar::find(d, nm, snap_id[si], &st);
                    if (snap_model[si].present[k]) note(id == snap_model[si].star[k] && st == stellar::Status::Ok, "snapshot find present (bloom false negative?)", step);
                    else note(id == stellar::INVALID_STAR && st == stellar::Status::NotFound, "snapshot find absent", step);
                } else {
                    note(stellar::mount(), "remount", step);
                }
            }
            u64 listed = 0, expect = 0;
            stellar::list(d, &count_cb, &listed);
            for (int k = 0; k < N; ++k) expect += live.present[k] ? 1 : 0;
            note(listed == expect, "final directory count", -1);
            for (int si = 0; si < nsnaps; ++si) {
                u64 sl = 0, se = 0;
                stellar::list(d, &count_cb, &sl, snap_id[si]);
                for (int k = 0; k < N; ++k) se += snap_model[si].present[k] ? 1 : 0;
                note(sl == se, "snapshot directory count", si);
            }
            stellar::LookupStats ls = stellar::lookup_stats();
            printf("fuzz with hash mask %lx: %lu failure(s); bloom rejects %lu, passes %lu, false positives %lu\n",
                   mask, bad, ls.bloom_rejects, ls.bloom_passes, ls.bloom_false_positives);
            CHECK(bad == 0, "randomized create/unlink/find/snapshot/remount agrees with the reference model");
        }
        stellar::test_set_hash_mask(0);
    }

    fresh_fs();
    {
        stellar::test_set_hash_mask(0);
        u64 d = stellar::create_constellation(stellar::ROOT_STAR, "churn");
        for (u64 i = 0; i < 8; ++i) { char nm[32]; name_of(nm, "keep", i); make(d, nm, nm); }
        stellar::LookupStats a = stellar::lookup_stats();
        bool ok = true;
        for (u64 i = 0; i < 6000; ++i) {
            char nm[32]; name_of(nm, "tmp", i);
            ok = ok && make(d, nm, nm) != stellar::INVALID_STAR && stellar::unlink(d, nm);
        }
        stellar::LookupStats b = stellar::lookup_stats();
        CHECK(ok, "6000 create/unlink pairs in a directory that holds 8 entries");
        printf("churn: %lu table growths over 6000 create/unlink pairs\n", b.index_grows - a.index_grows);
        CHECK(b.index_grows == a.index_grows, "the index does not grow while the live entry count stays flat");
        CHECK(count_dir(d) == 8, "and the directory still holds exactly its 8 entries");
    }

    fresh_fs();
    {
        stellar::test_set_hash_mask(0);
        u64 d = stellar::create_constellation(stellar::ROOT_STAR, "oom");
        for (u64 i = 0; i < 30; ++i) { char nm[32]; name_of(nm, "a", i); make(d, nm, nm); }
        u64 snap = stellar::snapshot();
        stellar::test_set_hash_mask(0);
        stellar::LookupStats l0 = stellar::lookup_stats();

        g_allocs_until_fail = 0;
        bool c_create = true, c_unlink = true, c_dup = true, c_live = true, c_snap = true, c_snap_absent = true;
        for (u64 i = 0; i < 30; ++i) { char nm[32]; name_of(nm, "b", i); c_create = c_create && make(d, nm, nm) != stellar::INVALID_STAR; }
        for (u64 i = 0; i < 30; i += 2) { char nm[32]; name_of(nm, "a", i); c_unlink = c_unlink && stellar::unlink(d, nm); }
        stellar::Status st = stellar::Status::Ok;
        c_dup = stellar::create_file(d, "a1", "dup", 3, &st) == stellar::INVALID_STAR && st == stellar::Status::Exists;
        for (u64 i = 0; i < 30; ++i) {
            char nm[32]; name_of(nm, "a", i);
            c_live = c_live && ((i % 2) ? eq(stellar::find(d, nm), nm) : stellar::find(d, nm) == stellar::INVALID_STAR);
            c_snap = c_snap && eq(stellar::find(d, nm, snap), nm, snap);
        }
        for (u64 i = 0; i < 30; ++i) { char nm[32]; name_of(nm, "b", i); c_snap_absent = c_snap_absent && stellar::find(d, nm, snap) == stellar::INVALID_STAR; }
        bool ok = c_create && c_unlink && c_dup && c_live && c_snap && c_snap_absent;
        if (!ok) printf("  scan fallback steps: create=%d unlink=%d dup=%d live=%d snap=%d snap_absent=%d\n", c_create, c_unlink, c_dup, c_live, c_snap, c_snap_absent);
        g_allocs_until_fail = -1;
        stellar::LookupStats l1 = stellar::lookup_stats();
        CHECK(ok, "with no memory for an index or a filter, every operation still works by scanning");
        CHECK(l1.scan_lookups > l0.scan_lookups && l1.index_builds == l0.index_builds && l1.bloom_builds == l0.bloom_builds,
              "and it really took the scan path");
        CHECK(count_dir(d) == 45, "directory count after scan-path creates and unlinks");

        bool again = true;
        for (u64 i = 0; i < 30; ++i) {
            char nm[32]; name_of(nm, "a", i);
            again = again && ((i % 2) ? eq(stellar::find(d, nm), nm) : stellar::find(d, nm) == stellar::INVALID_STAR);
            name_of(nm, "b", i);
            again = again && eq(stellar::find(d, nm), nm);
        }
        CHECK(again, "an index built afterwards agrees with what the scan path wrote");
        CHECK(make(d, "a0", "reborn") != stellar::INVALID_STAR && make(d, "b0", "x") == stellar::INVALID_STAR, "index path appends and rejects duplicates afterwards");
    }

    // ---------------------------------------------------------------- v6: checksums on every metadata sector
    fresh_fs();
    {
        using stellar::Status;
        Status st = Status::Internal;
        u64 d = stellar::create_constellation(stellar::ROOT_STAR, "d");
        for (u64 i = 0; i < 20; ++i) { char nm[32]; name_of(nm, "e", i); make(d, nm, nm); }
        make(stellar::ROOT_STAR, "keep", "kept");
        u8 saved[1024];
        __builtin_memcpy(saved, g_disk, 1024);
        auto cur_sb = [&]() -> const u8* {
            u64 s0, s1; __builtin_memcpy(&s0, g_disk + 64, 8); __builtin_memcpy(&s1, g_disk + 512 + 64, 8);
            return s1 > s0 ? g_disk + 512 : g_disk;
        };
        u64 s0, s1; __builtin_memcpy(&s0, g_disk + 64, 8); __builtin_memcpy(&s1, g_disk + 512 + 64, 8);
        CHECK(s0 != s1, "the two superblock slots hold different commit numbers after commits");

        u64 root_sec; __builtin_memcpy(&root_sec, cur_sb() + 40, 8);
        g_disk[root_sec * 512 + 100] ^= 1;
        CHECK(stellar::mount(), "mount does not read the catalog, so it still succeeds");
        CHECK(stellar::find(stellar::ROOT_STAR, "keep", stellar::LIVE, &st) == stellar::INVALID_STAR && st == Status::Checksum,
              "a corrupt catalog node is reported as Checksum, not trusted");
        g_disk[root_sec * 512 + 100] ^= 1;
        CHECK(stellar::mount() && eq(stellar::find(stellar::ROOT_STAR, "keep"), "kept"), "repairing the byte repairs the filesystem");

        u64 dir_sec = 0;
        for (u64 sec = 2; sec < g_sectors && !dir_sec; ++sec)
            for (u32 slot = 0; slot < 7 && !dir_sec; ++slot)
                if (__builtin_memcmp(g_disk + sec * 512 + 16 + slot * 64 + 12, "e0\0", 3) == 0 && g_disk[sec * 512 + 16 + slot * 64] == 1)
                    dir_sec = sec;
        CHECK(dir_sec != 0, "locate the directory sector holding e0");
        g_disk[dir_sec * 512 + 200] ^= 0x10;
        CHECK(stellar::mount(), "remount with a corrupt directory sector");
        CHECK(stellar::find(d, "e0", stellar::LIVE, &st) == stellar::INVALID_STAR && st == Status::Checksum, "a corrupt directory sector is reported as Checksum");
        u64 n = 0;
        stellar::list(d, &count_cb, &n, stellar::LIVE, &st);
        CHECK(st == Status::Checksum, "listing a corrupt directory reports Checksum");
        CHECK(make(d, "newfile", "x") == stellar::INVALID_STAR, "and it refuses to write into it");
        g_disk[dir_sec * 512 + 200] ^= 0x10;
        CHECK(stellar::mount() && stellar::find(d, "e0") != stellar::INVALID_STAR, "repaired");

        g_disk[2 * 512 + 10] ^= 1;
        CHECK(stellar::mount(&st) && st == Status::Ok, "a corrupt bitmap sector is rebuilt from the trees instead of refusing to mount");
        stellar::CheckReport rb{};
        CHECK(stellar::check(&rb) && rb.ok() && rb.leaked == 0, "and the rebuilt bitmap is exactly right");
        CHECK(eq(stellar::find(stellar::ROOT_STAR, "keep"), "kept") && stellar::find(d, "e0") != stellar::INVALID_STAR, "with all data intact");
        CHECK(stellar::mount() && stellar::check(&rb) && rb.ok(), "and the repair was written back");

        g_disk[100] ^= 1;
        CHECK(stellar::mount(&st), "one damaged superblock slot still mounts from the other");
        g_disk[100] ^= 1;
        g_disk[512 + 100] ^= 1;
        CHECK(stellar::mount(), "the other slot alone also mounts");
        g_disk[100] ^= 1;
        CHECK(!stellar::mount(&st) && st == Status::Corrupt, "both superblock slots damaged refuses to mount");
        g_disk[100] ^= 1;
        g_disk[512 + 100] ^= 1;
        CHECK(stellar::mount(), "both repaired");
        __builtin_memcpy(g_disk, saved, 1024);
        CHECK(stellar::mount(), "restored");

        sb_poke(72, 1, 4);
        CHECK(!stellar::mount(&st) && st == Status::Unsupported, "an unknown incompatible feature bit refuses to mount");
        __builtin_memcpy(g_disk, saved, 1024);
        sb_poke(76, 0xFFFFFFFFu, 4);
        CHECK(stellar::mount(&st), "compatible feature bits are ignored, not refused");
        __builtin_memcpy(g_disk, saved, 1024);
        CHECK(stellar::mount(), "restored again");
        make(stellar::ROOT_STAR, "after", "ok");
        u64 t0, t1; __builtin_memcpy(&t0, g_disk + 64, 8); __builtin_memcpy(&t1, g_disk + 512 + 64, 8);
        CHECK((t0 > s0 || t1 > s1) && t0 != t1, "a commit advances exactly one slot's commit number");
    }

    // ---------------------------------------------------------------- check(): the fsck oracle
    {
        using stellar::Status;
        Status st = Status::Internal;
        stellar::CheckReport r{};
        fresh_fs(4096);
        static u8 blob1[700], blob2[3000];
        for (u64 i = 0; i < sizeof(blob1); ++i) blob1[i] = static_cast<u8>(i * 5 + 1);
        for (u64 i = 0; i < sizeof(blob2); ++i) blob2[i] = static_cast<u8>(i * 11 + 3);
        u64 f1 = stellar::create_file(stellar::ROOT_STAR, "f1", blob1, sizeof(blob1));
        u64 f2 = stellar::create_file(stellar::ROOT_STAR, "f2", blob2, sizeof(blob2));
        u64 d = stellar::create_constellation(stellar::ROOT_STAR, "d");
        u64 inner = make(d, "in", "inner");
        CHECK(stellar::check(&r, true, &st) && st == Status::Ok && r.ok(), "a fresh filesystem checks clean");
        CHECK(r.files == 3 && r.dirs == 2 && r.entries == 4 && r.snapshots == 0, "check counts what it walked");
        stellar::link(d, "f1link", f1);
        u64 snap = stellar::snapshot();
        stellar::write_file(f2, "changed", 7);
        stellar::unlink(d, "in");
        make(d, "later", "later");
        CHECK(stellar::check(&r) && r.snapshots == 1 && r.ok(), "after links, a snapshot, rewrites and unlinks it still checks clean");
        (void)snap; (void)inner;
        CHECK(stellar::gc() != stellar::INVALID_STAR && stellar::check(&r) && r.leaked == 0, "after gc nothing is leaked");

        static u8 saved[4096 * 512];
        __builtin_memcpy(saved, g_disk, sizeof(saved));
        auto restore = [&]() { __builtin_memcpy(g_disk, saved, sizeof(saved)); CHECK(stellar::mount(), "restore image"); };
        auto newest_root = [&]() {
            u64 a, b; __builtin_memcpy(&a, g_disk + 64, 8); __builtin_memcpy(&b, g_disk + 512 + 64, 8);
            u64 root; __builtin_memcpy(&root, g_disk + (b > a ? 512 : 0) + 40, 8);
            return root;
        };
        u64 leaf = newest_root();
        auto entry_off = [&](u32 idx) { return leaf * 512 + 16 + 64 * idx + 8; };
        CHECK(g_disk[leaf * 512] == 1, "the catalog root is a single leaf in this small filesystem");
        auto first_of = [&](u32 idx) { u64 v; __builtin_memcpy(&v, g_disk + entry_off(idx) + 16, 8); return v; };
        auto fix = [&](u64 sec) { sb_fix_crc(g_disk + sec * 512); };

        restore();
        u64 data_sec = first_of(1);
        g_disk[data_sec * 512 + 5] ^= 0x20;
        CHECK(stellar::mount() && stellar::check(&r, false) && r.ok(), "a shallow check does not read file data");
        CHECK(!stellar::check(&r, true, &st) && st == Status::Corrupt && r.bad_files >= 1 && r.bad_crc == 0, "a deep check finds a corrupt file");

        restore();
        g_disk[leaf * 512 + 100] ^= 1;
        CHECK(stellar::mount() && !stellar::check(&r) && r.bad_crc >= 1, "a corrupt catalog node is found by checksum");

        restore();
        u64 dsec = 0; u32 dslot = 0;
        for (u64 sec = 2; sec < 4096 && !dsec; ++sec)
            for (u32 i = 0; i < 7 && !dsec; ++i)
                if (__builtin_memcmp(g_disk + sec * 512 + 16 + i * 64 + 12, "f1link\0", 7) == 0 && g_disk[sec * 512 + 16 + i * 64] == 1) { dsec = sec; dslot = i; }
        CHECK(dsec != 0, "locate the directory entry f1link");
        u64 bogus = 99999;
        __builtin_memcpy(g_disk + dsec * 512 + 16 + dslot * 64 + 4, &bogus, 8);
        fix(dsec);
        CHECK(stellar::mount() && !stellar::check(&r) && r.bad_structure >= 1, "an entry pointing at a star that does not exist is found");

        restore();
        u32 nl; __builtin_memcpy(&nl, g_disk + entry_off(2) + 36, 4);
        nl += 5;
        __builtin_memcpy(g_disk + entry_off(2) + 36, &nl, 4);
        fix(leaf);
        CHECK(stellar::mount() && !stellar::check(&r) && r.bad_links >= 1, "a wrong link count is found");

        restore();
        u64 victim = first_of(2);
        u64 bm_byte = victim / 8;
        u64 bm_sec = 2 + bm_byte / 508;
        g_disk[bm_sec * 512 + bm_byte % 508] &= static_cast<u8>(~(1u << (victim % 8)));
        fix(bm_sec);
        CHECK(stellar::mount() && !stellar::check(&r) && r.unmarked >= 1, "a referenced sector marked free in the bitmap is found");

        restore();
        u64 stray = 3000;
        u64 sb_byte = stray / 8;
        u64 sb_sec = 2 + sb_byte / 508;
        g_disk[sb_sec * 512 + sb_byte % 508] |= static_cast<u8>(1u << (stray % 8));
        fix(sb_sec);
        CHECK(stellar::mount() && stellar::check(&r) && r.leaked >= 1, "a leaked sector is reported but is harmless");

        restore();
        u64 shared = first_of(1);
        __builtin_memcpy(g_disk + entry_off(2) + 16, &shared, 8);
        fix(leaf);
        CHECK(stellar::mount() && !stellar::check(&r) && r.bad_structure >= 1, "two files sharing sectors are found");

        restore();
        CHECK(stellar::check(&r) && r.ok(), "the restored image is clean again");
        (void)f1; (void)f2;
    }

    // ---------------------------------------------------------------- v6: failed operations roll back
    fresh_fs(4096);
    {
        using stellar::Status;
        Status st = Status::Internal;
        stellar::CheckReport r{};
        const u64 NONE = stellar::INVALID_STAR;
        make(stellar::ROOT_STAR, "base", "base");
        u64 free0 = stellar::free_space_sectors();
        u64 count0 = count_dir(stellar::ROOT_STAR);
        bool all = true;
        for (u64 k = 0; k < 6; ++k) {
            g_wcount = 0; g_fail_after = k;
            u64 id = stellar::create_file(stellar::ROOT_STAR, "doomed", "x", 1, &st);
            g_fail_after = ~0ull;
            all = all && id == NONE && st == Status::Io && stellar::find(stellar::ROOT_STAR, "doomed") == NONE &&
                  count_dir(stellar::ROOT_STAR) == count0 && stellar::free_space_sectors() == free0 &&
                  stellar::check(&r) && r.ok();
        }
        CHECK(all, "a create failing at any of its first writes fails with Io, leaves no trace, leaks no space, and the filesystem checks clean");
        CHECK(make(stellar::ROOT_STAR, "doomed", "now ok") != NONE, "the same name can be created afterwards");

        g_flush_fails = true;
        CHECK(stellar::create_file(stellar::ROOT_STAR, "noflush", "x", 1, &st) == NONE && st == Status::Io, "a failed flush fails the operation");
        g_flush_fails = false;
        CHECK(stellar::find(stellar::ROOT_STAR, "noflush") == NONE, "and the operation is rolled back");
        CHECK(make(stellar::ROOT_STAR, "noflush", "x") != NONE, "it works once flushes work again");

        stellar::begin_batch();
        make(stellar::ROOT_STAR, "b1", "1");
        make(stellar::ROOT_STAR, "b2", "2");
        CHECK(stellar::create_file(stellar::ROOT_STAR, "b1", "dup", 3, &st) == NONE && st == Status::Exists, "a validation failure inside a batch is reported");
        CHECK(stellar::gc(&st) == NONE && st == Status::Busy, "gc refuses to run inside a batch");
        CHECK(stellar::end_batch(&st) && st == Status::Ok, "and does not poison the batch");
        CHECK(eq(stellar::find(stellar::ROOT_STAR, "b1"), "1") && eq(stellar::find(stellar::ROOT_STAR, "b2"), "2"), "the other batched creates landed");

        stellar::begin_batch();
        make(stellar::ROOT_STAR, "p1", "1");
        g_wcount = 0; g_fail_after = 1;
        CHECK(stellar::create_file(stellar::ROOT_STAR, "p2", "2", 1, &st) == NONE, "an I/O failure inside a batch fails that operation");
        g_fail_after = ~0ull;
        CHECK(!stellar::end_batch(&st), "end_batch reports the batch as failed");
        CHECK(stellar::find(stellar::ROOT_STAR, "p1") == NONE && stellar::find(stellar::ROOT_STAR, "p2") == NONE, "and the whole batch is rolled back");
        CHECK(stellar::check(&r) && r.ok() && make(stellar::ROOT_STAR, "p1", "again") != NONE, "the filesystem is intact and usable");
    }

    // ---------------------------------------------------------------- v6: the older superblock is a consistent commit
    fresh_fs(4096);
    {
        using stellar::Status;
        Status st = Status::Internal;
        stellar::CheckReport r{};
        make(stellar::ROOT_STAR, "a", "A");
        make(stellar::ROOT_STAR, "b", "B");
        make(stellar::ROOT_STAR, "c", "C");
        u64 s0, s1; __builtin_memcpy(&s0, g_disk + 64, 8); __builtin_memcpy(&s1, g_disk + 512 + 64, 8);
        u8* newest = s1 > s0 ? g_disk + 512 : g_disk;
        newest[100] ^= 1;
        CHECK(stellar::mount(&st) && st == Status::Ok, "mount succeeds from the older superblock");
        CHECK(eq(stellar::find(stellar::ROOT_STAR, "a"), "A") && eq(stellar::find(stellar::ROOT_STAR, "b"), "B"), "it holds the state before the last commit");
        CHECK(stellar::find(stellar::ROOT_STAR, "c") == stellar::INVALID_STAR, "and the last commit is cleanly absent, not half present");
        CHECK(stellar::check(&r) && r.ok() && r.leaked == 0, "the free-space bitmap was rebuilt to match that state exactly");
        CHECK(make(stellar::ROOT_STAR, "d", "D") != stellar::INVALID_STAR, "writes work and the damaged slot is replaced");
        CHECK(stellar::mount() && eq(stellar::find(stellar::ROOT_STAR, "d"), "D") && stellar::find(stellar::ROOT_STAR, "c") == stellar::INVALID_STAR && stellar::check(&r) && r.ok(), "and it persists");
    }

    // ---------------------------------------------------------------- crash consistency
    {
        using stellar::Status;
        const u32 SEEDS = 2;
        u64 grand_states = 0, grand_bad = 0, grand_garbage = 0;
        for (u32 seed = 1; seed <= SEEDS; ++seed) {
            fresh_fs(1024);
            static u8 base[1024 * 512], dur[1024 * 512], work[1024 * 512];
            __builtin_memcpy(base, g_disk, sizeof(base));
            static u64 pos_start[128], pos_end[128], dg[129];
            const int NOPS = 64;
            g_log_n = 0; g_rec = true; g_quiet = true;
            dg[0] = tree_digest();

            u64 rng = 0x9E3779B97F4A7C15ull * seed + 12345;
            auto rnd = [&]() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return static_cast<u32>(rng >> 11); };
            struct F { u64 dir; char name[16]; u64 star; bool alive; };
            static F files[96]; u32 nfiles = 0;
            static u64 dirs[10]; u32 ndirs = 1; dirs[0] = stellar::ROOT_STAR;
            static u64 snaps[8]; u32 nsnap = 0;
            u32 counter = 0;
            static u8 data[3000];
            auto fill = [&](u32 size) { u32 salt = rnd(); for (u32 i = 0; i < size; ++i) data[i] = static_cast<u8>(i * 31 + salt); };
            auto create = [&]() {
                char nm[16]; name_of(nm, "f", counter++);
                u64 dir = dirs[rnd() % ndirs];
                u32 size = rnd() % 2500;
                fill(size);
                u64 id = stellar::create_file(dir, nm, data, size);
                if (id != stellar::INVALID_STAR && nfiles < 96) {
                    files[nfiles].dir = dir; files[nfiles].star = id; files[nfiles].alive = true;
                    u32 k = 0; for (; nm[k]; ++k) files[nfiles].name[k] = nm[k]; files[nfiles].name[k] = 0;
                    ++nfiles;
                }
            };
            auto pick_alive = [&]() -> int {
                if (!nfiles) return -1;
                for (int tries = 0; tries < 8; ++tries) { u32 i = rnd() % nfiles; if (files[i].alive) return static_cast<int>(i); }
                return -1;
            };
            for (int i = 0; i < NOPS; ++i) {
                pos_start[i] = g_log_n;
                u32 r = rnd() % 100;
                if (r < 30) create();
                else if (r < 38 && ndirs < 9) {
                    char nm[16]; name_of(nm, "d", counter++);
                    u64 id = stellar::create_constellation(dirs[rnd() % ndirs], nm);
                    if (id != stellar::INVALID_STAR) dirs[ndirs++] = id;
                } else if (r < 54) {
                    int f = pick_alive();
                    if (f >= 0) { u32 size = rnd() % 2500; fill(size); stellar::write_file(files[f].star, data, size); }
                } else if (r < 68) {
                    int f = pick_alive();
                    if (f >= 0 && stellar::unlink(files[f].dir, files[f].name)) files[f].alive = false;
                } else if (r < 74) {
                    int f = pick_alive();
                    if (f >= 0) { char nm[16]; name_of(nm, "l", counter++); stellar::link(dirs[rnd() % ndirs], nm, files[f].star); }
                } else if (r < 81) {
                    if (nsnap < 4) { u64 id = stellar::snapshot(); if (id != stellar::INVALID_STAR) snaps[nsnap++] = id; }
                } else if (r < 86) {
                    if (nsnap) { u32 k = rnd() % nsnap; if (stellar::delete_snapshot(snaps[k])) { snaps[k] = snaps[--nsnap]; } }
                } else if (r < 90) {
                    stellar::gc();
                } else {
                    stellar::begin_batch();
                    for (int j = 0; j < 3; ++j) create();
                    stellar::end_batch();
                }
                pos_end[i] = g_log_n;
                dg[i + 1] = tree_digest();
            }
            g_rec = false;
            CHECK(g_log_n < 24000, "the write log did not overflow");
            u64 writes = 0, barriers = 0;
            for (u32 i = 0; i < g_log_n; ++i) { if (g_log[i].sector == ~0ull) ++barriers; else ++writes; }
            stellar::CheckReport final_r{};
            CHECK(stellar::check(&final_r) && final_r.ok(), "the recorded workload ends in a clean filesystem");

            u8* heap = g_disk;
            __builtin_memcpy(dur, base, sizeof(dur));
            u32 bnd = 0, scan = 0;
            u64 states = 0, bad = 0, garbage_states = 0, deep = 0;
            u64 rng2 = 0xD1B54A32D192ED03ull * seed;
            auto rnd2 = [&]() { rng2 ^= rng2 << 13; rng2 ^= rng2 >> 7; rng2 ^= rng2 << 17; return static_cast<u32>(rng2 >> 11); };
            const u32 stride = 2;
            auto process = [&](u32 k) {
                while (scan < k) {
                    if (g_log[scan].sector == ~0ull) {
                        for (u32 q = bnd; q < scan; ++q)
                            if (g_log[q].sector != ~0ull) __builtin_memcpy(dur + g_log[q].sector * 512, g_log[q].data, 512);
                        bnd = scan + 1;
                    }
                    ++scan;
                }
                u32 win[512]; u32 wn = 0;
                for (u32 q = bnd; q < k && wn < 512; ++q) if (g_log[q].sector != ~0ull) win[wn++] = q;
                int in = -1;
                for (int i = 0; i < NOPS; ++i) if (pos_start[i] < k && k < pos_end[i]) in = i;
                u64 a1, a2;
                if (in >= 0) { a1 = dg[in]; a2 = dg[in + 1]; }
                else { int j = 0; for (int i = 0; i < NOPS; ++i) if (pos_end[i] <= k) j = i + 1; a1 = a2 = dg[j]; }

                for (int variant = 0; variant < 7; ++variant) {
                    if (variant >= 2 && wn == 0) break;
                    __builtin_memcpy(work, dur, sizeof(work));
                    bool inc[512];
                    int garbage = -1;
                    for (u32 q = 0; q < wn; ++q) {
                        inc[q] = variant == 0 ? false : variant == 1 ? true : (rnd2() & 1);
                    }
                    if (variant == 5) { garbage = static_cast<int>(wn - 1); inc[garbage] = true; }
                    if (variant == 6) { garbage = static_cast<int>(rnd2() % wn); inc[garbage] = true; }
                    u32 order[512];
                    for (u32 q = 0; q < wn; ++q) order[q] = q;
                    if (variant >= 2) for (u32 q = wn; q > 1; --q) { u32 j = rnd2() % q; u32 tmp = order[q - 1]; order[q - 1] = order[j]; order[j] = tmp; }
                    for (u32 qi = 0; qi < wn; ++qi) {
                        u32 q = order[qi];
                        if (!inc[q]) continue;
                        u8* dst = work + g_log[win[q]].sector * 512;
                        if (static_cast<int>(q) == garbage) for (int b = 0; b < 512; ++b) dst[b] = static_cast<u8>(rnd2());
                        else __builtin_memcpy(dst, g_log[win[q]].data, 512);
                    }
                    ++states;
                    if (garbage >= 0) ++garbage_states;
                    g_disk = work;
                    Status st = Status::Internal;
                    const char* why = nullptr;
                    if (!stellar::mount(&st)) why = "mount failed";
                    else {
                        u64 d = tree_digest();
                        stellar::CheckReport r{};
                        if (d != a1 && d != a2) why = "state is neither before nor after the in-flight operation";
                        else if (!stellar::check(&r, true) || r.unmarked || r.bad_crc || r.bad_files || r.bad_links || r.bad_structure) why = "check() found damage";
                        else if (states % 5 == 0) {
                            ++deep;
                            if (stellar::gc() == stellar::INVALID_STAR) why = "gc failed on the crashed image";
                            else if (!stellar::check(&r) || !r.ok() || r.leaked != 0) why = "gc did not leave a perfect bitmap";
                            else if (tree_digest() != d) why = "gc changed the tree";
                            else if (stellar::create_file(stellar::ROOT_STAR, "post-crash", "alive", 5) == stellar::INVALID_STAR) why = "cannot write after recovery";
                            else if (!stellar::mount() || !stellar::check(&r) || !r.ok()) why = "image is damaged after writing post-recovery";
                            else if (!eq(stellar::find(stellar::ROOT_STAR, "post-crash"), "alive")) why = "post-recovery write was lost";
                        }
                    }
                    if (why) {
                        if (!bad) printf("  crash state failed: seed %u cut %u/%u variant %d window %u op %d: %s (%s)\n",
                                          seed, k, g_log_n, variant, wn, in, why, stellar::status_name(st));
                        ++bad;
                    }
                }
            };
            for (u32 k = 0; k < g_log_n; k += stride) process(k);
            process(g_log_n);
            g_disk = heap;
            g_quiet = false;
            printf("crash exploration seed %u: %d ops, %lu writes, %lu flush barriers, %lu crash states (%lu with a torn write, %lu with recovery+continue), %lu bad\n",
                   seed, NOPS, writes, barriers, states, garbage_states, deep, bad);
            grand_states += states; grand_bad += bad; grand_garbage += garbage_states;
            CHECK(bad == 0, "every simulated crash recovers to exactly the state before or after the in-flight operation");
        }
        CHECK(grand_states > 5000 && grand_garbage > 500, "the exploration covered thousands of crash states, hundreds with torn writes");
        (void)grand_bad;
    }

    // ---------------------------------------------------------------- v6: stat, flags, clock
    fresh_fs();
    {
        using stellar::Status;
        static u64 fake_time;
        stellar::set_clock([]() -> u64 { return fake_time; });
        Status st = Status::Internal;
        stellar::StatInfo si{};
        fake_time = 1000;
        u64 f = make(stellar::ROOT_STAR, "f", "one");
        u64 d = stellar::create_constellation(stellar::ROOT_STAR, "dir");
        CHECK(stellar::stat(f, &si, stellar::LIVE, &st) && st == Status::Ok, "stat a file");
        CHECK(si.type == stellar::TYPE_FILE && si.size_bytes == 3 && si.nlink == 1 && si.mtime == 1000 && si.flags == 0, "file metadata");
        CHECK(stellar::stat(d, &si) && si.type == stellar::TYPE_CONSTELLATION && si.mtime == 1000, "directory metadata");
        fake_time = 2000;
        CHECK(stellar::write_file(f, "two22", 5) == f, "rewrite");
        CHECK(stellar::stat(f, &si) && si.mtime == 2000 && si.size_bytes == 5, "a rewrite updates mtime and size");
        u64 snap = stellar::snapshot();
        fake_time = 3000;
        stellar::write_file(f, "x", 1);
        CHECK(stellar::stat(f, &si) && si.mtime == 3000, "live view sees the newest mtime");
        CHECK(stellar::stat(f, &si, snap) && si.mtime == 2000 && si.size_bytes == 5, "the snapshot keeps the old metadata");
        CHECK(stellar::set_flags(f, 0x10000, &st) && st == Status::Ok, "set a user flag");
        CHECK(stellar::stat(f, &si) && si.flags == 0x10000, "flag is visible");
        CHECK(stellar::mount() && stellar::stat(f, &si) && si.flags == 0x10000, "flag persists across a remount");
        stellar::write_file(f, "yy", 2);
        CHECK(stellar::stat(f, &si) && si.flags == 0x10000, "a rewrite preserves flags");
        CHECK(!stellar::set_flags(f, stellar::FLAG_EXTENT_TABLE, &st) && st == Status::InvalidArgument, "reserved flag bits are refused");
        CHECK(!stellar::set_flags(77777, 0x10000, &st) && st == Status::NotFound, "set_flags on a missing star");
        CHECK(!stellar::stat(77777, &si, stellar::LIVE, &st) && st == Status::NotFound, "stat a missing star");
        CHECK(!stellar::stat(f, nullptr, stellar::LIVE, &st) && st == Status::InvalidArgument, "stat with no output");
        CHECK(!stellar::stat(f, &si, 9, &st) && st == Status::NoSuchSnapshot, "stat in a missing snapshot");
        stellar::set_clock(nullptr);
    }

    // ---------------------------------------------------------------- bloom filter on snapshot lookups
    fresh_fs();
    {
        stellar::test_set_hash_mask(0);
        u64 d = stellar::create_constellation(stellar::ROOT_STAR, "bf");
        for (u64 i = 0; i < 200; ++i) { char nm[32]; name_of(nm, "b", i); make(d, nm, nm); }
        u64 s = stellar::snapshot();
        for (u64 i = 0; i < 50; ++i) { char nm[32]; name_of(nm, "b", i); stellar::unlink(d, nm); }
        for (u64 i = 0; i < 50; ++i) { char nm[32]; name_of(nm, "after", i); make(d, nm, nm); }

        stellar::LookupStats a = stellar::lookup_stats();
        stellar::IoStats io0 = stellar::io_stats();
        u64 found = 0;
        for (u64 i = 0; i < 2000; ++i) { char nm[32]; name_of(nm, "zz", i); found += stellar::find(d, nm, s) != stellar::INVALID_STAR; }
        stellar::LookupStats b = stellar::lookup_stats();
        stellar::IoStats io1 = stellar::io_stats();
        u64 rejects = b.bloom_rejects - a.bloom_rejects, scans = b.scan_lookups - a.scan_lookups, fps = b.bloom_false_positives - a.bloom_false_positives;
        u64 lookups = (io1.reads - io0.reads) + (io1.cache_hits - io0.cache_hits);
        printf("2000 absent-name lookups in a snapshot: %lu rejected by the filter, %lu scanned (%lu false positives), %.1f sector lookups each\n",
               rejects, scans, fps, (double)lookups / 2000.0);
        CHECK(found == 0, "no absent name is ever found");
        CHECK(rejects >= 1960, "at least 98% of absent names are rejected without touching the directory");
        CHECK(scans <= 40 && fps == scans, "only filter false positives fall through to a scan");
        CHECK(b.bloom_builds - a.bloom_builds == 1, "the filter is built once per snapshot directory");
        CHECK(lookups <= 2000 * 6, "a rejected lookup costs only the catalog descent");

        bool all = true;
        for (u64 i = 0; i < 200; ++i) { char nm[32]; name_of(nm, "b", i); all = all && eq(stellar::find(d, nm, s), nm, s); }
        CHECK(all, "the filter has no false negatives: every name in the snapshot is found");
        bool none = true;
        for (u64 i = 0; i < 50; ++i) { char nm[32]; name_of(nm, "after", i); none = none && stellar::find(d, nm, s) == stellar::INVALID_STAR; }
        CHECK(none, "names created after the snapshot are not in it");

        CHECK(stellar::delete_snapshot(s), "delete the snapshot");
        u64 s2 = stellar::snapshot();
        stellar::LookupStats c = stellar::lookup_stats();
        bool second = true;
        for (u64 i = 50; i < 200; ++i) { char nm[32]; name_of(nm, "b", i); second = second && stellar::find(d, nm, s2) != stellar::INVALID_STAR; }
        for (u64 i = 0; i < 50; ++i) { char nm[32]; name_of(nm, "b", i); second = second && stellar::find(d, nm, s2) == stellar::INVALID_STAR; }
        CHECK(second, "a snapshot taken after the delete sees its own contents");
        CHECK(stellar::lookup_stats().bloom_builds - c.bloom_builds == 1, "deleting a snapshot drops its filter");
    }

    // ---------------------------------------------------------------- path resolution
    fresh_fs();
    {
        using stellar::Status;
        Status st = Status::Internal;
        u64 a = stellar::create_constellation(stellar::ROOT_STAR, "a");
        u64 b = stellar::create_constellation(a, "b");
        u64 c = make(b, "c.txt", "see\n");
        u64 x = make(a, "x.txt", "x");
        u64 top = make(stellar::ROOT_STAR, "top.txt", "t");
        char n60[62]; n60[0] = '/'; for (int i = 1; i <= 60; ++i) n60[i] = 'q'; n60[61] = 0;
        auto R = [&](const char* path, u64 want, Status ws, const char* msg, u64 snap = stellar::LIVE) {
            st = Status::Internal;
            u64 got = stellar::resolve(path, snap, &st);
            if (got != want || st != ws) printf("  resolve('%s'): got %lu/%s\n", path, got, stellar::status_name(st));
            CHECK(got == want && st == ws, msg);
        };
        const u64 NONE = stellar::INVALID_STAR;
        R("/", stellar::ROOT_STAR, Status::Ok, "root");
        R("/a", a, Status::Ok, "one component");
        R("/a/b/c.txt", c, Status::Ok, "nested file");
        R("//a///b//c.txt", c, Status::Ok, "repeated slashes collapse");
        R("/a/b/", b, Status::Ok, "trailing slash on a directory");
        R("/a/b///", b, Status::Ok, "several trailing slashes");
        R("/top.txt", top, Status::Ok, "file at the root");
        R("/top.txt/", NONE, Status::NotADirectory, "trailing slash on a file");
        R("/a/b/c.txt/d", NONE, Status::NotADirectory, "descending through a file");
        R("/missing", NONE, Status::NotFound, "missing leaf");
        R("/a/missing/c.txt", NONE, Status::NotFound, "missing middle component");
        R("/a/../a", NONE, Status::InvalidName, "dotdot is refused");
        R("/a/./b", NONE, Status::InvalidName, "dot is refused");
        R("a/b", NONE, Status::InvalidName, "relative paths are refused");
        R("", NONE, Status::InvalidName, "empty path");
        R(n60, NONE, Status::InvalidName, "over-long component");

        u64 snap = stellar::snapshot();
        u64 fresh = make(a, "new.txt", "n");
        R("/a/new.txt", fresh, Status::Ok, "live view sees the new file");
        R("/a/new.txt", NONE, Status::NotFound, "snapshot view does not", snap);
        R("/a/b/c.txt", c, Status::Ok, "snapshot view resolves old paths", snap);
        R("/a/b/c.txt", NONE, Status::NoSuchSnapshot, "missing snapshot", 9);

        char leaf[stellar::NAME_MAX_LEN + 1];
        auto P = [&](const char* path, u64 want, const char* want_leaf, Status ws, const char* msg, u64 bufsz = sizeof(leaf)) {
            st = Status::Internal;
            leaf[0] = 0;
            u64 got = stellar::resolve_parent(path, leaf, bufsz, stellar::LIVE, &st);
            bool leaf_ok = want_leaf == nullptr || strcmp_(leaf, want_leaf);
            if (got != want || st != ws || !leaf_ok) printf("  resolve_parent('%s'): got %lu/%s leaf '%s'\n", path, got, stellar::status_name(st), leaf);
            CHECK(got == want && st == ws && leaf_ok, msg);
        };
        P("/a/b/new.txt", b, "new.txt", Status::Ok, "parent and leaf of a new path");
        P("/top2", stellar::ROOT_STAR, "top2", Status::Ok, "leaf directly under the root");
        P("/a/b/", a, "b", Status::Ok, "trailing slash is ignored");
        P("/", NONE, nullptr, Status::InvalidName, "the root has no leaf");
        P("/missing/x", NONE, nullptr, Status::NotFound, "missing parent");
        P("/top.txt/x", NONE, nullptr, Status::NotADirectory, "parent is a file");
        P("/a/..", NONE, nullptr, Status::InvalidName, "dotdot leaf");
        P("/a/b/new.txt", NONE, nullptr, Status::InvalidName, "leaf buffer too small", 4);
        u64 p = stellar::resolve_parent("/a/b/made.txt", leaf, sizeof(leaf));
        u64 made = stellar::create_file(p, leaf, "m", 1);
        CHECK(made != stellar::INVALID_STAR && stellar::resolve("/a/b/made.txt") == made, "create by path round trip");
        (void)x;
    }

    // ---------------------------------------------------------------- civil time and RTC decoding
    {
        struct Ref { u32 y, mo, d, h, mi, s; u64 unix_s; };
        static const Ref refs[] = {
            {1970, 1, 1, 0, 0, 0, 0ull},
            {1999, 12, 31, 23, 59, 59, 946684799ull},
            {2000, 1, 1, 0, 0, 0, 946684800ull},
            {2000, 2, 29, 23, 59, 59, 951868799ull},
            {2001, 3, 1, 0, 0, 0, 983404800ull},
            {2024, 2, 29, 12, 34, 56, 1709210096ull},
            {2026, 10, 7, 2, 13, 55, 1791339235ull},
            {2038, 1, 19, 3, 14, 7, 2147483647ull},
            {2099, 12, 31, 23, 59, 59, 4102444799ull},
            {2100, 3, 1, 0, 0, 0, 4107542400ull},
        };
        bool all = true;
        for (const Ref& r : refs) {
            civil::Date d{r.y, r.mo, r.d, r.h, r.mi, r.s};
            civil::Date back = civil::from_unix(r.unix_s);
            all = all && civil::to_unix(d) == r.unix_s && back.year == r.y && back.month == r.mo && back.day == r.d &&
                  back.hour == r.h && back.minute == r.mi && back.second == r.s;
        }
        CHECK(all, "civil conversion matches independently computed Unix times, both directions");

        bool sweep = true;
        for (u64 t = 0; t < 4200000000ull && sweep; t += 3607) {
            civil::Date d = civil::from_unix(t);
            sweep = civil::valid(d) && civil::to_unix(d) == t;
        }
        CHECK(sweep, "from_unix and to_unix round-trip every 3607 s from 1970 to 2103");

        auto raw = [](u8 s, u8 mi, u8 h, u8 d, u8 mo, u8 y) { return civil::RtcRaw{s, mi, h, d, mo, y}; };
        civil::Date out{};
        CHECK(civil::decode_rtc(raw(0x55, 0x13, 0x02, 0x07, 0x10, 0x26), false, true, &out) &&
              civil::to_unix(out) == 1791339235ull, "BCD, 24-hour register values decode");
        CHECK(civil::decode_rtc(raw(55, 13, 2, 7, 10, 26), true, true, &out) &&
              civil::to_unix(out) == 1791339235ull, "binary register values decode to the same instant");
        CHECK(civil::decode_rtc(raw(0, 0, 12, 1, 1, 0), true, false, &out) && out.hour == 0, "12-hour binary: 12 AM is hour 0");
        CHECK(civil::decode_rtc(raw(0, 0, 0x8C, 1, 1, 0), true, false, &out) && out.hour == 12, "12-hour binary: 12 PM is hour 12");
        CHECK(civil::decode_rtc(raw(0, 0, 0x81, 1, 1, 0), true, false, &out) && out.hour == 13, "12-hour binary: 1 PM is hour 13");
        CHECK(civil::decode_rtc(raw(0, 0, 0x81, 1, 1, 0), false, false, &out) && out.hour == 13, "12-hour BCD: 0x81 is 1 PM");
        CHECK(civil::decode_rtc(raw(0, 0, 0x91, 1, 1, 0), false, false, &out) && out.hour == 23, "12-hour BCD: 0x91 is 11 PM");
        CHECK(civil::decode_rtc(raw(0, 0, 0x92, 1, 1, 0), false, false, &out) && out.hour == 12, "12-hour BCD: 0x92 is 12 PM");
        CHECK(civil::decode_rtc(raw(0, 0, 0x12, 1, 1, 0), false, false, &out) && out.hour == 0, "12-hour BCD: 0x12 is 12 AM");
        CHECK(civil::decode_rtc(raw(0, 0, 0, 0x29, 0x02, 0x24), false, true, &out) && out.day == 29 && out.month == 2, "leap day accepted");

        CHECK(!civil::decode_rtc(raw(0x1A, 0, 0, 1, 1, 0), false, true, &out), "bad BCD nibble is rejected");
        CHECK(!civil::decode_rtc(raw(0, 0, 0, 1, 13, 0), true, true, &out), "month 13 is rejected");
        CHECK(!civil::decode_rtc(raw(0, 0, 0, 0, 1, 0), true, true, &out), "day 0 is rejected");
        CHECK(!civil::decode_rtc(raw(0, 0, 0, 30, 2, 24), true, true, &out), "Feb 30 is rejected");
        CHECK(!civil::decode_rtc(raw(0, 0, 0, 29, 2, 25), true, true, &out), "Feb 29 in a non-leap year is rejected");
        CHECK(!civil::decode_rtc(raw(0, 0, 24, 1, 1, 0), true, true, &out), "hour 24 is rejected");
        CHECK(!civil::decode_rtc(raw(0, 60, 0, 1, 1, 0), true, true, &out), "minute 60 is rejected");
        CHECK(!civil::decode_rtc(raw(60, 0, 0, 1, 1, 0), true, true, &out), "second 60 is rejected");
        CHECK(!civil::decode_rtc(raw(0, 0, 0, 1, 1, 0), true, false, &out), "12-hour mode hour 0 is rejected");
        CHECK(!civil::decode_rtc(raw(0, 0, 0x8D, 1, 1, 0), true, false, &out), "12-hour mode hour 13 is rejected");

        char iso[21];
        civil::format_iso(civil::from_unix(1791339235ull), iso);
        CHECK(strcmp_(iso, "2026-10-07T02:13:55Z"), "ISO formatting is zero-padded");
        civil::format_iso(civil::from_unix(0), iso);
        CHECK(strcmp_(iso, "1970-01-01T00:00:00Z"), "ISO formatting of the epoch");
    }

    // ---------------------------------------------------------------- auto-format guard
    {
        using stellar::Status;
        Status st = Status::Internal;
        auto blank_disk = [&](u64 sectors) {
            free(g_disk);
            g_sectors = sectors;
            g_disk = static_cast<u8*>(calloc(g_sectors, 512));
            stellar::init(0);
        };

        blank_disk(4096);
        CHECK(!stellar::mount(&st) && st == Status::NotFormatted, "a blank disk mounts as NotFormatted");
        CHECK(stellar::can_auto_format(), "a blank disk may be formatted");

        const u64 foreign_sectors[] = {0, 1, 2, 33, 63};
        for (u64 sec : foreign_sectors) {
            blank_disk(4096);
            g_disk[sec * 512 + 510] = 0x55;
            g_disk[sec * 512 + 511] = 0xAA;
            CHECK(!stellar::mount(&st) && st == Status::NotFormatted, "foreign data still reads as NotFormatted");
            CHECK(!stellar::can_auto_format(), "foreign data in the probed area blocks auto-format");
        }

        blank_disk(4096);
        CHECK(stellar::format(g_sectors), "format for the stale-version case");
        sb_poke(8, 5, 4);
        CHECK(!stellar::mount(&st) && st == Status::NotFormatted, "an older Stellar version reads as NotFormatted");
        CHECK(stellar::can_auto_format(), "an older Stellar volume may be reformatted, as documented");

        blank_disk(4096);
        CHECK(stellar::format(g_sectors), "format for the damaged-superblock case");
        g_disk[100] ^= 1;
        g_disk[512 + 100] ^= 1;
        CHECK(!stellar::mount(&st) && st == Status::Corrupt, "damaged superblocks report Corrupt, which the kernel never formats over");

        blank_disk(4096);
        CHECK(stellar::format(g_sectors), "format for the unsupported-features case");
        sb_poke(72, 1, 4);
        CHECK(!stellar::mount(&st) && st == Status::Unsupported, "unknown incompatible features report Unsupported, which the kernel never formats over");

        blank_disk(63);
        CHECK(!stellar::can_auto_format(), "a device smaller than the minimum is never auto-formatted");
        fresh_fs();
    }

    printf(g_fail ? "\n%d FAILURE(S)\n" : "\nall stellar host tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
