// Scaling benchmark for Stellar FS: formats, mounts, scans and collects on sparse virtual disks of growing size and
// reports host time, device commands and the RAM the free-space bitmap needs. It links the filesystem's own source.
#include <cosmos/types.hpp>
#include <cosmos/blockdev.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/orbital.hpp>
#include <cosmos/stellar.hpp>

extern "C" {
int printf(const char*, ...);
void* aligned_alloc(unsigned long, unsigned long);
void* mmap(void*, unsigned long, int, int, int, long);
int munmap(void*, unsigned long);
int clock_gettime(int, void*);
}

struct Timespec { long sec, nsec; };
static double now_ms() {
    Timespec t;
    clock_gettime(1, &t);
    return t.sec * 1000.0 + t.nsec / 1e6;
}

static u8* g_disk;
static u64 g_sectors;
static u64 g_cmd_read1, g_cmd_readn, g_cmd_write1, g_cmd_writen, g_sec_read, g_sec_write;

namespace blockdev {
bool read_sector(u64 s, void* b) { if (s >= g_sectors) return false; ++g_cmd_read1; ++g_sec_read; __builtin_memcpy(b, g_disk + s * 512, 512); return true; }
bool write_sector(u64 s, const void* b) { if (s >= g_sectors) return false; ++g_cmd_write1; ++g_sec_write; __builtin_memcpy(g_disk + s * 512, b, 512); return true; }
bool read_sectors(u64 s, u64 n, void* b) { if (s + n > g_sectors) return false; ++g_cmd_readn; g_sec_read += n; __builtin_memcpy(b, g_disk + s * 512, n * 512); return true; }
bool write_sectors(u64 s, u64 n, const void* b) { if (s + n > g_sectors) return false; ++g_cmd_writen; g_sec_write += n; __builtin_memcpy(g_disk + s * 512, b, n * 512); return true; }
u64 capacity_sectors() { return g_sectors; }
bool flush() { return true; }
}
namespace universe {
u64 alloc(int order) { return reinterpret_cast<u64>(aligned_alloc(4096, PAGE_SIZE << order)); }
void free(u64, int) {}
}
namespace serial {
void printf(const char*, ...) {}
void writeln(const char*) {}
}
namespace orbital {
void yield() {}
void yield_contended() {}
u64 self_token() { return 1; }
}

static void reset_counters() { g_cmd_read1 = g_cmd_readn = g_cmd_write1 = g_cmd_writen = g_sec_read = g_sec_write = 0; }

int main() {
    const u64 sizes[] = {1ull << 20, 1ull << 22, 1ull << 24};
    printf("%-9s %-24s %10s %12s %12s\n", "disk", "operation", "ms", "commands", "sectors");
    for (u64 n : sizes) {
        g_sectors = n;
        g_disk = static_cast<u8*>(mmap(nullptr, n * 512, 3, 0x2 | 0x20 | 0x4000, -1, 0));
        if (g_disk == reinterpret_cast<u8*>(~0ull)) { printf("mmap failed for %lu sectors\n", n); return 1; }
        stellar::init(0);
        char label[16];
        double gib = n * 512.0 / (1024.0 * 1024.0 * 1024.0);
        { int i = 0; double v = gib; if (v >= 1) { label[i++] = static_cast<char>('0' + static_cast<int>(v) % 10); } else { label[i++] = '0'; label[i++] = '.'; label[i++] = '5'; } label[i++] = ' '; label[i++] = 'G'; label[i++] = 'i'; label[i++] = 'B'; label[i] = 0; }
        auto row = [&](const char* op, double ms) {
            printf("%-9s %-24s %10.2f %12lu %12lu\n", label, op, ms, g_cmd_read1 + g_cmd_readn + g_cmd_write1 + g_cmd_writen, g_sec_read + g_sec_write);
        };

        reset_counters();
        double t = now_ms();
        if (!stellar::format(n)) { printf("format failed\n"); return 1; }
        row("format", now_ms() - t);

        for (u64 i = 0; i < 200; ++i) {
            char nm[16]; u32 l = 0; u64 v = i; nm[l++] = 'f'; char d[8]; u32 dl = 0; if (!v) d[dl++] = '0'; while (v) { d[dl++] = static_cast<char>('0' + v % 10); v /= 10; } while (dl) nm[l++] = d[--dl]; nm[l] = 0;
            stellar::create_file(stellar::ROOT_STAR, nm, "payload", 7);
        }

        reset_counters();
        t = now_ms();
        if (!stellar::mount()) { printf("mount failed\n"); return 1; }
        row("mount", now_ms() - t);

        printf("%-9s %-24s %10lu KiB\n", label, "bitmap RAM", stellar::bitmap_ram_bytes() / 1024);

        reset_counters();
        t = now_ms();
        u64 free_sec = 0;
        for (int i = 0; i < 20; ++i) free_sec = stellar::free_space_sectors();
        row("free_space_sectors x20", now_ms() - t);
        (void)free_sec;

        reset_counters();
        t = now_ms();
        stellar::gc();
        row("gc", now_ms() - t);

        reset_counters();
        t = now_ms();
        stellar::CheckReport r{};
        stellar::check(&r, false);
        row("check (shallow)", now_ms() - t);
        munmap(g_disk, n * 512);
        printf("\n");
    }
    return 0;
}
