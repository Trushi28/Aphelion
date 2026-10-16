#include <cosmos/types.hpp>
#include "../kernel/src/mm/pmm.cpp"
#include "../kernel/src/mm/vmm.cpp"
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>

static constexpr u64 ARENA = 64ull << 20;

struct Rng {
    u64 s = 88172645463325252ull;
    u64 operator()() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
};

struct Block { u64 phys; int order; };

static int g_failures = 0;

static void expect(bool cond, const char* what) {
    printf("%-58s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) ++g_failures;
}

static void shape_of(u64* out) { universe::count_blocks(out); }

static bool same_shape(const u64* a, const u64* b) {
    for (int o = 0; o <= universe::MAX_ORDER; ++o) if (a[o] != b[o]) return false;
    return true;
}

static void pmm_tests(u64 hhdm, const universe::MemmapEntry* map, u64 count) {
    using namespace universe;
    expect(locked_check(), "pmm: invariants after seeding, adjacent regions merged");
    expect(selftest(), "pmm: selftest after init");
    u64 seeded[MAX_ORDER + 1];
    shape_of(seeded);
    reclaim_bootloader_regions(map, count);
    expect(locked_check(), "pmm: invariants after reclaim, regions merged");
    expect(selftest(), "pmm: selftest after reclaim");

    u64 base_shape[MAX_ORDER + 1];
    shape_of(base_shape);
    u64 base_free = free_bytes();

    static Block live[1000000];
    static u8 mark[ARENA >> 12];
    u64 live_n = 0;
    Rng rng;
    bool overlap = false, checks = true;
    for (int i = 0; i < 400000; ++i) {
        if (live_n == 0 || rng() % 100 < 55) {
            int o = rng() % 100 < 80 ? static_cast<int>(rng() % 3) : static_cast<int>(rng() % 9);
            u64 p = alloc(o);
            if (!p) continue;
            for (u64 k = 0; k < (1ull << o); ++k) {
                if (mark[(p >> 12) + k]) overlap = true;
                mark[(p >> 12) + k] = 1;
            }
            memset(reinterpret_cast<void*>(hhdm + p), 0xA5, 64);
            live[live_n++] = { p, o };
        } else {
            u64 j = rng() % live_n;
            Block b = live[j];
            live[j] = live[--live_n];
            for (u64 k = 0; k < (1ull << b.order); ++k) mark[(b.phys >> 12) + k] = 0;
            free(b.phys, b.order);
        }
        if (i % 40000 == 0 && !locked_check()) checks = false;
    }
    for (u64 i = live_n; i > 1; --i) {
        u64 j = rng() % i;
        Block t = live[i - 1]; live[i - 1] = live[j]; live[j] = t;
    }
    for (u64 i = 0; i < live_n; ++i) free(live[i].phys, live[i].order);

    u64 end_shape[MAX_ORDER + 1];
    shape_of(end_shape);
    expect(!overlap, "pmm: randomized stress never hands out overlapping blocks");
    expect(checks && locked_check(), "pmm: invariants hold throughout the stress");
    expect(free_bytes() == base_free, "pmm: all memory returned after the stress");
    expect(same_shape(base_shape, end_shape), "pmm: free-list shape fully coalesces back");

    pid_t pid = fork();
    if (pid == 0) { u64 p = alloc(0); free(p, 0); free(p, 0); _exit(0); }
    int st = 0;
    waitpid(pid, &st, 0);
    expect(WIFSIGNALED(st), "pmm: a double free is fatal");
}

int main() {
    void* arena = mmap(nullptr, ARENA, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    u64 hhdm = reinterpret_cast<u64>(arena);
    universe::MemmapEntry map[] = {
        { 0x0,       0x100000,  2 },
        { 0x100000,  0x700000,  0 },
        { 0x800000,  0x800000,  0 },
        { 0x1000000, 0x400000,  5 },
        { 0x1400000, 0x400000,  2 },
        { 0x1800000, 0x1800000, 0 },
    };
    universe::init(map, 6, hhdm);
    pmm_tests(hhdm, map, 6);

    constellation::init(hhdm, 0x100000, 0xffffffff80000000ull, 0x4000, 0x3000000);
    expect(constellation::selftest(), "vmm: translate, PAT-preserving split, argument checks");
    constellation::seal();
    return g_failures ? 1 : 0;
}
