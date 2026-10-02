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
        for (u64 i = 0; i < 300; ++i) {
            char nm[32]; name_of(nm, "m", i);
            make(stellar::ROOT_STAR, nm, nm);
        }
        auto b = stellar::io_stats();
        u64 lookups = (b.reads - a.reads) + (b.cache_hits - a.cache_hits);
        printf("300 creates in one directory: %lu sector lookups (%.1f per create)\n", lookups, (double)lookups / 300.0);
        CHECK(lookups <= 300 * 20, "directory append cost does not grow with directory size");
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

    printf(g_fail ? "\n%d FAILURE(S)\n" : "\nall stellar host tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
