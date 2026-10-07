// Host-side mkfs / fsck / ls / cat for Stellar FS images. It links the filesystem's own source
// (kernel/src/fs/stellar.cpp) against a file-backed block device, so fsck runs exactly the verifier
// the kernel has.
#include <cosmos/types.hpp>
#include <cosmos/blockdev.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/orbital.hpp>
#include <cosmos/stellar.hpp>
#include <cosmos/civil.hpp>

extern "C" {
int printf(const char*, ...);
int vprintf(const char*, __builtin_va_list);
int puts(const char*);
int putchar(int);
int open(const char*, int, ...);
int close(int);
long pread(int, void*, unsigned long, long);
long pwrite(int, const void*, unsigned long, long);
long lseek(int, long, int);
int ftruncate(int, long);
int fsync(int);
int strcmp(const char*, const char*);
void* aligned_alloc(unsigned long, unsigned long);
void exit(int);
}

static int g_fd = -1;
static u64 g_sectors = 0;

namespace blockdev {
bool read_sector(u64 s, void* b) { return s < g_sectors && pread(g_fd, b, 512, static_cast<long>(s * 512)) == 512; }
bool write_sector(u64 s, const void* b) { return s < g_sectors && pwrite(g_fd, b, 512, static_cast<long>(s * 512)) == 512; }
bool read_sectors(u64 s, u64 n, void* b) { return s + n <= g_sectors && pread(g_fd, b, n * 512, static_cast<long>(s * 512)) == static_cast<long>(n * 512); }
bool write_sectors(u64 s, u64 n, const void* b) { return s + n <= g_sectors && pwrite(g_fd, b, n * 512, static_cast<long>(s * 512)) == static_cast<long>(n * 512); }
u64 capacity_sectors() { return g_sectors; }
bool flush() { return fsync(g_fd) == 0; }
}
namespace universe {
u64 alloc(int order) { return reinterpret_cast<u64>(aligned_alloc(4096, PAGE_SIZE << order)); }
void free(u64, int) {}
}
namespace serial {
void printf(const char* fmt, ...) { __builtin_va_list ap; __builtin_va_start(ap, fmt); vprintf(fmt, ap); __builtin_va_end(ap); }
void writeln(const char* s) { puts(s); }
}
namespace orbital {
void yield() {}
void yield_contended() {}
u64 self_token() { return 1; }
}

static u64 parse_u64(const char* s) { u64 v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + static_cast<u64>(*s++ - '0'); return v; }

static void ls_cb(const char* name, u64 star, u32 type, void* ctx) {
    u64 depth = *static_cast<u64*>(ctx);
    stellar::StatInfo si{};
    stellar::stat(star, &si);
    for (u64 i = 0; i < depth; ++i) printf("  ");
    if (type == stellar::TYPE_CONSTELLATION) printf("%s/  (star %llu", name, star);
    else printf("%s  (star %llu, %llu bytes, %u link(s)", name, star, si.size_bytes, si.nlink);
    if (si.mtime) {
        char iso[21];
        civil::format_iso(civil::from_unix(si.mtime / 1000), iso);
        printf(", mtime %s", iso);
    }
    printf(")\n");
    if (type == stellar::TYPE_CONSTELLATION) { u64 d = depth + 1; stellar::list(star, &ls_cb, &d); }
}

static int usage() {
    printf("usage: stellarfs mkfs <image> [sectors]\n       stellarfs fsck <image> [--repair]\n       stellarfs ls <image>\n       stellarfs cat <image> <path>\n");
    return 2;
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const char* cmd = argv[1];
    const char* img = argv[2];
    bool mk = strcmp(cmd, "mkfs") == 0;
    g_fd = open(img, mk ? (2 | 0100) : 2, 0644);   // O_RDWR (| O_CREAT for mkfs)
    if (g_fd < 0) { printf("cannot open %s\n", img); return 2; }
    long size = lseek(g_fd, 0, 2);
    if (mk && argc > 3 && parse_u64(argv[3]) * 512 > static_cast<u64>(size)) { ftruncate(g_fd, static_cast<long>(parse_u64(argv[3]) * 512)); size = lseek(g_fd, 0, 2); }
    g_sectors = static_cast<u64>(size) / 512;
    stellar::init(0);
    if (mk) {
        if (g_sectors < 64) { printf("image too small (need at least 64 sectors)\n"); return 2; }
        stellar::Status st;
        if (!stellar::format(g_sectors, &st)) { printf("format failed: %s\n", stellar::status_name(st)); return 1; }
        printf("formatted %s: %llu sectors\n", img, g_sectors);
        return 0;
    }
    stellar::Status st;
    if (!stellar::mount(&st)) { printf("mount failed: %s\n", stellar::status_name(st)); return 1; }
    if (strcmp(cmd, "fsck") == 0) {
        bool repair = argc > 3 && strcmp(argv[3], "--repair") == 0;
        stellar::CheckReport r{};
        bool ok = stellar::check(&r, true, &st);
        printf("fsck: %llu node(s), %llu dir(s), %llu file(s), %llu entr%s, %llu snapshot(s)\n", r.nodes, r.dirs, r.files, r.entries, r.entries == 1 ? "y" : "ies", r.snapshots);
        printf("      bad checksums %llu, bad files %llu, referenced-but-free %llu, bad links %llu, bad structure %llu, leaked sectors %llu\n",
               r.bad_crc, r.bad_files, r.unmarked, r.bad_links, r.bad_structure, r.leaked);
        if (repair && r.leaked) {
            u64 freed = stellar::gc();
            printf("      repair: gc reclaimed %llu leaked sector(s)\n", freed);
            ok = stellar::check(&r, true) && ok;
        }
        printf(ok ? "fsck: clean\n" : "fsck: DAMAGED (%s)\n", stellar::status_name(st));
        return ok ? 0 : 1;
    }
    if (strcmp(cmd, "ls") == 0) { u64 d = 0; stellar::list(stellar::ROOT_STAR, &ls_cb, &d); return 0; }
    if (strcmp(cmd, "cat") == 0 && argc > 3) {
        u64 star = stellar::resolve(argv[3], stellar::LIVE, &st);
        if (star == stellar::INVALID_STAR) { printf("%s: %s\n", argv[3], stellar::status_name(st)); return 1; }
        static u8 buf[1 << 20];
        u64 n = stellar::read_file(star, buf, sizeof(buf), stellar::LIVE, &st);
        if (n == stellar::READ_ERROR) { printf("%s: %s\n", argv[3], stellar::status_name(st)); return 1; }
        for (u64 i = 0; i < n; ++i) putchar(buf[i]);
        return 0;
    }
    return usage();
}
