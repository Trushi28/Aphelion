#include <cosmos/types.hpp>
#include <cosmos/blockdev.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/stellar.hpp>

extern "C" {
int printf(const char*, ...);
int vprintf(const char*, __builtin_va_list);
int puts(const char*);
void* aligned_alloc(unsigned long, unsigned long);
void* calloc(unsigned long, unsigned long);
}

static u8* g_disk;
static u64 g_sectors;

namespace blockdev {
bool read_sector(u64 s, void* b) { if (s >= g_sectors) return false; __builtin_memcpy(b, g_disk + s * 512, 512); return true; }
bool write_sector(u64 s, const void* b) { if (s >= g_sectors) return false; __builtin_memcpy(g_disk + s * 512, b, 512); return true; }
bool read_sectors(u64 s, u64 n, void* b) { if (s + n > g_sectors) return false; __builtin_memcpy(b, g_disk + s * 512, n * 512); return true; }
bool write_sectors(u64 s, u64 n, const void* b) { if (s + n > g_sectors) return false; __builtin_memcpy(g_disk + s * 512, b, n * 512); return true; }
u64 capacity_sectors() { return g_sectors; }
}

namespace universe {
u64 alloc(int order) {
    unsigned long bytes = PAGE_SIZE << order;
    return reinterpret_cast<u64>(aligned_alloc(4096, bytes));
}
void free(u64, int) {}
}

namespace serial {
void printf(const char* fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vprintf(fmt, ap);
    __builtin_va_end(ap);
}
void writeln(const char* s) { puts(s); }
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

static void fresh_fs() {
    g_sectors = 131072;
    g_disk = static_cast<u8*>(calloc(g_sectors, 512));
    stellar::init(0);
    CHECK(!stellar::mount(), "blank disk must not mount");
    CHECK(stellar::format(g_sectors), "format");
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

    printf(g_fail ? "\n%d FAILURE(S)\n" : "\nall stellar host tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
