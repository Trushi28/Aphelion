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
void free(void*);
}

static u8* g_disk;
static u64 g_sectors;

static u64 g_wcount = 0;
static u64 g_fail_after = ~0ull;

namespace blockdev {
bool read_sector(u64 s, void* b) { if (s >= g_sectors) return false; __builtin_memcpy(b, g_disk + s * 512, 512); return true; }
bool write_sector(u64 s, const void* b) {
    if (s >= g_sectors || g_wcount >= g_fail_after) return false;
    ++g_wcount;
    __builtin_memcpy(g_disk + s * 512, b, 512);
    return true;
}
bool read_sectors(u64 s, u64 n, void* b) { if (s + n > g_sectors) return false; __builtin_memcpy(b, g_disk + s * 512, n * 512); return true; }
bool write_sectors(u64 s, u64 n, const void* b) {
    if (s + n > g_sectors) return false;
    u64 ok_n = n;
    if (g_wcount + n > g_fail_after) ok_n = g_fail_after > g_wcount ? g_fail_after - g_wcount : 0;
    __builtin_memcpy(g_disk + s * 512, b, ok_n * 512);
    g_wcount += ok_n;
    return ok_n == n;
}
u64 capacity_sectors() { return g_sectors; }
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

static void fresh_fs(u64 sectors = 131072) {
    free(g_disk);
    g_sectors = sectors;
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
        CHECK(stellar::read_file(tmp, sink, sizeof(sink)) == 0, "dead star reads nothing");
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
        CHECK(stellar::read_file(shared, sink, sizeof(sink)) == 0, "last unlink kills the star");
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
        for (u64 i = 0; i < 300; ++i) {
            if (i % 100 == 0) mark[i / 100] = stellar::io_stats();
            char nm[32]; name_of(nm, "m", i);
            make(stellar::ROOT_STAR, nm, nm);
        }
        auto b = stellar::io_stats();
        mark[3] = b;
        u64 lookups = (b.reads - a.reads) + (b.cache_hits - a.cache_hits);
        u64 early_writes = mark[1].writes - mark[0].writes;
        u64 late_writes = mark[3].writes - mark[2].writes;
        u64 chain = (300 + 6) / 7 + 1;
        printf("300 creates in one directory: %lu sector lookups (%.1f per create), %lu disk reads, writes first/last 100: %lu/%lu\n",
               lookups, (double)lookups / 300.0, b.reads - a.reads, early_writes, late_writes);
        CHECK(late_writes <= early_writes + early_writes / 4, "disk writes per create stay flat as the directory grows");
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
        CHECK(stellar::read_file(a1, sink, sizeof(sink)) == 0 && stellar::find(stellar::ROOT_STAR, "dup") == stellar::INVALID_STAR,
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
        u8 good[512];
        __builtin_memcpy(good, g_disk, 512);

        struct Poke { u64 off; u64 val; int bytes; };
        struct Case { const char* name; Poke a; Poke b; };
        const u64 total = g_sectors;
        const Case cases[] = {
            {"sector size is not 512", {12, 1024, 4}, {0, 0, 0}},
            {"total sectors larger than the device", {16, total + 1, 8}, {0, 0, 0}},
            {"total sectors zero", {16, 0, 8}, {0, 0, 0}},
            {"total sectors tiny", {16, 4, 8}, {0, 0, 0}},
            {"bitmap does not start at sector 1", {24, 2, 8}, {0, 0, 0}},
            {"bitmap sector count too large", {32, 9999, 8}, {0, 0, 0}},
            {"bitmap sector count zero", {32, 0, 8}, {0, 0, 0}},
            {"bitmap sector count off by one", {32, 33, 8}, {0, 0, 0}},
            {"catalog root zero", {40, 0, 8}, {0, 0, 0}},
            {"catalog root inside the bitmap", {40, 1, 8}, {0, 0, 0}},
            {"catalog root past the end", {40, total, 8}, {0, 0, 0}},
            {"epoch zero", {56, 0, 8}, {0, 0, 0}},
            {"snapshot count over the table", {64, 25, 4}, {0, 0, 0}},
            {"snapshot count huge", {64, 0xFFFFFFFFu, 4}, {0, 0, 0}},
            {"snapshot root past the end", {64, 1, 4}, {72, total + 5, 8}},
            {"snapshot root inside the bitmap", {64, 1, 4}, {72, 3, 8}},
        };
        for (const Case& c : cases) {
            __builtin_memcpy(g_disk, good, 512);
            const Poke pokes[2] = {c.a, c.b};
            for (const Poke& p : pokes) {
                if (!p.bytes) continue;
                __builtin_memcpy(g_disk + p.off, &p.val, p.bytes);
            }
            CHECK(!stellar::mount(), c.name);
            CHECK(stellar::find(stellar::ROOT_STAR, "keep") == stellar::INVALID_STAR, "a rejected mount leaves the filesystem unmounted");
            CHECK(stellar::create_file(stellar::ROOT_STAR, "x", "x", 1) == stellar::INVALID_STAR, "writes fail after a rejected mount");
        }
        __builtin_memcpy(g_disk, good, 512);
        CHECK(stellar::mount(), "the untouched superblock still mounts");
        CHECK(eq(stellar::find(stellar::ROOT_STAR, "keep"), "data"), "and the data is intact");

        __builtin_memcpy(g_disk, good, 512);
        u32 snaps = 24;
        __builtin_memcpy(g_disk + 64, &snaps, 4);
        u64 root_copy;
        __builtin_memcpy(&root_copy, g_disk + 40, 8);
        for (u32 i = 0; i < 24; ++i) __builtin_memcpy(g_disk + 72 + 16 * i, &root_copy, 8);
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
            bool clean = op.run();
            u64 total = g_wcount;
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
        u64 probe = make(stellar::ROOT_STAR, "probe", "p");
        u64 w = g_wcount;
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

    printf(g_fail ? "\n%d FAILURE(S)\n" : "\nall stellar host tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
