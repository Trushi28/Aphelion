#include <cosmos/types.hpp>
#include <cosmos/tlb.hpp>
#include "../kernel/src/mm/pmm.cpp"
#include "../kernel/src/mm/kheap.cpp"
#include "../kernel/src/mm/vmm.cpp"
#include "../kernel/src/mm/dma.cpp"
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <pthread.h>

namespace constellation { bool translate(u64, Mapping*); }

static u64 g_hhdm_for_probe = 0;
static u32 g_tlb_calls = 0;
static u64 g_tlb_mask = 0, g_tlb_addr = 0, g_tlb_bytes = 0;
static u64 g_fake_online = 1;

namespace tlb {
void poll() {}
u32 core_index() { return 0; }
u64 online_mask() { return g_fake_online; }
u32 online_count() { return 1; }
void flush(u64 mask, u64 addr, u64 bytes) { ++g_tlb_calls; g_tlb_mask = mask; g_tlb_addr = addr; g_tlb_bytes = bytes; }
bool selftest_probe(u64 addr, u64* out, u32 slots) {
    constellation::Mapping m{};
    if (slots < 1 || !constellation::translate(addr, &m)) return false;
    out[0] = *reinterpret_cast<u64*>(g_hhdm_for_probe + m.phys);
    return true;
}
}

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


static bool dies_in_child(void (*fn)()) {
    pid_t pid = fork();
    if (pid == 0) { fn(); _exit(0); }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFSIGNALED(st);
}

static void die_small_double_free() { void* p = kheap::alloc(64); kheap::free(p); kheap::free(p); }
static void die_large_double_free() { void* p = kheap::alloc(100000); kheap::free(p); kheap::free(p); }
static void die_interior_small() { u8* p = static_cast<u8*>(kheap::alloc(64)); kheap::free(p + 8); }
static void die_interior_large() { u8* p = static_cast<u8*>(kheap::alloc(100000)); kheap::free(p + 4096); }
static void die_wild() { kheap::free(reinterpret_cast<void*>(0x1234)); }
static void die_unowned_page() { u64 p = universe::alloc(0); kheap::free(universe::to_virt(p)); }
static void die_write_after_free() {
    u8* p = static_cast<u8*>(kheap::alloc(100));
    kheap::free(p);
    p[40] = 0;
    for (int i = 0; i < 5000; ++i) kheap::alloc(100);
}
static void die_outside_direct_map() { kheap::free(reinterpret_cast<void*>(g_hhdm_for_probe + (200ull << 20))); }

struct Live { u8* p; u32 size; u8 tag; };

static bool fill_ok(const Live& l) {
    u32 n = l.size < 192 ? l.size : 192;
    for (u32 i = 0; i < n; ++i) if (l.p[i] != static_cast<u8>(l.tag + i)) return false;
    if (l.size > 192 && l.p[l.size - 1] != l.tag) return false;
    return true;
}

static void fill(Live& l) {
    u32 n = l.size < 192 ? l.size : 192;
    for (u32 i = 0; i < n; ++i) l.p[i] = static_cast<u8>(l.tag + i);
    if (l.size > 192) l.p[l.size - 1] = l.tag;
}

static u32 pick_size(Rng& rng) {
    u64 r = rng() % 1000;
    if (r < 600) return 1 + rng() % 128;
    if (r < 900) return 129 + rng() % 1920;
    if (r < 990) return 2049 + rng() % 30000;
    return 32768 + rng() % 200000;
}

static void heap_tests() {
    kheap::init();
    expect(kheap::ready(), "kheap: initialised");
    expect(kheap::selftest(), "kheap: boot self-test");
    kheap::Stats base = kheap::stats();

    static Live live[4096];
    u32 n = 0;
    Rng rng;
    bool corrupt = false, overlap = false, aligned = true;
    for (int i = 0; i < 300000; ++i) {
        if (n == 0 || (n < 4096 && rng() % 100 < 52)) {
            Live l;
            l.size = pick_size(rng);
            l.p = static_cast<u8*>(kheap::alloc(l.size));
            if (!l.p) continue;
            l.tag = static_cast<u8>(rng());
            if (reinterpret_cast<u64>(l.p) % 16) aligned = false;
            if (kheap::usable_size(l.p) < l.size) corrupt = true;
            fill(l);
            live[n++] = l;
        } else {
            u32 j = rng() % n;
            if (!fill_ok(live[j])) corrupt = true;
            kheap::free(live[j].p);
            live[j] = live[--n];
        }
        if (i % 50000 == 0 && !kheap::check()) corrupt = true;
    }
    for (u32 a = 0; a < n && !overlap; a += 7) {
        for (u32 b = a + 1; b < n && b < a + 64; ++b) {
            u64 pa = reinterpret_cast<u64>(live[a].p), pb = reinterpret_cast<u64>(live[b].p);
            if (pa < pb + live[b].size && pb < pa + live[a].size) { overlap = true; break; }
        }
    }
    for (u32 i = 0; i < n; ++i) {
        if (!fill_ok(live[i])) corrupt = true;
        kheap::free(live[i].p);
    }
    kheap::Stats end = kheap::stats();
    expect(!corrupt, "kheap: contents survive a randomized stress");
    expect(!overlap, "kheap: live objects never overlap");
    expect(aligned, "kheap: every pointer is 16-byte aligned");
    expect(kheap::check(), "kheap: slab lists consistent after the stress");
    expect(end.live_objects == base.live_objects && end.live_bytes == base.live_bytes && end.large_pages == base.large_pages,
           "kheap: every object returned");
    expect(end.slab_pages <= base.slab_pages + kheap::CLASS_COUNT * 4, "kheap: empty slabs are handed back to the buddy allocator");

    u64 free_before = universe::free_bytes();
    void* huge = kheap::alloc(5u << 20);
    expect(huge && kheap::usable_size(huge) == (5u << 20) && universe::free_bytes() == free_before - (5u << 20),
           "kheap: a 5 MiB block takes exactly 5 MiB, not the next power of two");
    kheap::free(huge);
    expect(universe::free_bytes() == free_before && universe::locked_check(), "kheap: the block returns whole and coalesces");
    expect(kheap::alloc(universe::PAGE_SIZE << universe::MAX_ORDER << 1) == nullptr, "kheap: an impossible size fails cleanly");
    expect(kheap::alloc(~0ull) == nullptr, "kheap: a size near the address-space limit fails cleanly");
    expect(kheap::stats().failed_allocs >= 2, "kheap: failed allocations are counted");

    expect(dies_in_child(die_small_double_free), "kheap: a small double free is fatal");
    expect(dies_in_child(die_large_double_free), "kheap: a large double free is fatal");
    expect(dies_in_child(die_interior_small), "kheap: freeing inside a small object is fatal");
    expect(dies_in_child(die_interior_large), "kheap: freeing inside a large block is fatal");
    expect(dies_in_child(die_wild), "kheap: freeing a wild pointer is fatal");
    expect(dies_in_child(die_unowned_page), "kheap: freeing a page the heap does not own is fatal");
    expect(dies_in_child(die_write_after_free), "kheap: a write to freed memory is caught on reuse");
    expect(dies_in_child(die_outside_direct_map), "kheap: freeing never-allocated direct-map memory is fatal");
}

static volatile u32 g_mt_errors = 0;
static volatile u32 g_mt_overlap = 0;
static u8 g_mt_mark[ARENA >> 12];

static void* mt_worker(void* arg) {
    u64 id = reinterpret_cast<u64>(arg);
    Rng rng;
    rng.s ^= (id + 1) * 0x9E3779B97F4A7C15ull;
    static thread_local Live live[256];
    u32 n = 0;
    struct Page { u64 phys; int order; };
    Page pages[32];
    u32 pn = 0;
    for (int i = 0; i < 60000; ++i) {
        u64 r = rng() % 100;
        if (r < 30) {
            if (pn < 32) {
                int o = static_cast<int>(rng() % 4);
                u64 p = universe::alloc(o);
                if (p) {
                    for (u64 k = 0; k < (1ull << o); ++k)
                        if (__atomic_exchange_n(&g_mt_mark[(p >> 12) + k], static_cast<u8>(id + 1), __ATOMIC_ACQ_REL))
                            __atomic_fetch_add(&g_mt_overlap, 1u, __ATOMIC_RELAXED);
                    pages[pn++] = { p, o };
                }
            }
        } else if (r < 45) {
            if (pn) {
                Page pg = pages[--pn];
                for (u64 k = 0; k < (1ull << pg.order); ++k) {
                    if (__atomic_load_n(&g_mt_mark[(pg.phys >> 12) + k], __ATOMIC_ACQUIRE) != static_cast<u8>(id + 1))
                        __atomic_fetch_add(&g_mt_errors, 1u, __ATOMIC_RELAXED);
                    __atomic_store_n(&g_mt_mark[(pg.phys >> 12) + k], 0, __ATOMIC_RELEASE);
                }
                universe::free(pg.phys, pg.order);
            }
        } else if (n == 0 || (n < 256 && r < 75)) {
            Live l;
            l.size = pick_size(rng);
            if (l.size > 40000) l.size = 1 + l.size % 5000;
            l.p = static_cast<u8*>(kheap::alloc(l.size));
            if (!l.p) continue;
            l.tag = static_cast<u8>(id * 61 + i);
            fill(l);
            live[n++] = l;
        } else {
            u32 j = rng() % n;
            if (!fill_ok(live[j])) __atomic_fetch_add(&g_mt_errors, 1u, __ATOMIC_RELAXED);
            kheap::free(live[j].p);
            live[j] = live[--n];
        }
    }
    for (u32 i = 0; i < n; ++i) {
        if (!fill_ok(live[i])) __atomic_fetch_add(&g_mt_errors, 1u, __ATOMIC_RELAXED);
        kheap::free(live[i].p);
    }
    for (u32 i = 0; i < pn; ++i) {
        for (u64 k = 0; k < (1ull << pages[i].order); ++k) __atomic_store_n(&g_mt_mark[(pages[i].phys >> 12) + k], 0, __ATOMIC_RELEASE);
        universe::free(pages[i].phys, pages[i].order);
    }
    return nullptr;
}

static void thread_tests() {
    constexpr int T = 4;
    u64 free_before = universe::free_bytes();
    kheap::Stats base = kheap::stats();
    pthread_t th[T];
    for (int i = 0; i < T; ++i) pthread_create(&th[i], nullptr, mt_worker, reinterpret_cast<void*>(static_cast<u64>(i)));
    for (int i = 0; i < T; ++i) pthread_join(th[i], nullptr);
    kheap::Stats end = kheap::stats();
    expect(g_mt_errors == 0, "threads: 4 workers never see foreign writes in their memory");
    expect(g_mt_overlap == 0, "threads: the buddy allocator never hands one page to two threads");
    expect(universe::locked_check(), "threads: buddy invariants hold after contention");
    expect(kheap::check(), "threads: heap invariants hold after contention");
    expect(end.live_objects == base.live_objects && end.live_bytes == base.live_bytes, "threads: the heap is balanced");
    u64 slab_growth = (end.slab_pages - base.slab_pages) * universe::PAGE_SIZE;
    expect(universe::free_bytes() + slab_growth == free_before, "threads: the buddy allocator is balanced");
}

static void shootdown_tests(u64 hhdm) {
    using namespace constellation;
    constexpr u64 VA = 0xFFFFE00000000000ull + 0x20000000ull;
    u64 f = universe::alloc_zeroed(0);
    u32 c0 = g_tlb_calls;
    expect(map_4k(VA, f, WRITABLE | NO_EXECUTE) && g_tlb_calls == c0, "tlb: mapping a fresh page needs no shootdown");
    g_fake_online = 0x7;
    expect(map_4k(VA, f, WRITABLE | NO_EXECUTE) && g_tlb_calls == c0 + 1 && g_tlb_mask == 0x7 && g_tlb_addr == VA && g_tlb_bytes == 0x1000,
           "tlb: remapping a present page shoots down every online core");
    expect(protect(VA, 0x1000, NO_EXECUTE) && g_tlb_calls == c0 + 2 && g_tlb_bytes == 0x1000, "tlb: protect shoots down");
    expect(unmap(VA, 0x1000) && g_tlb_calls == c0 + 3 && g_tlb_addr == VA, "tlb: unmap shoots down");
    u32 before = g_tlb_calls;
    expect(!unmap(VA, 0x1000) && g_tlb_calls == before, "tlb: unmapping nothing does nothing");

    expect(map_2m(VA & ~0x1FFFFFull, 0x1000000, WRITABLE | NO_EXECUTE), "tlb: 2 MiB page installed");
    u32 c1 = g_tlb_calls;
    expect(map_4k((VA & ~0x1FFFFFull) + 0x3000, f, WRITABLE | NO_EXECUTE) && g_tlb_calls == c1 + 1,
           "tlb: splitting and overwriting inside a huge page shoots down");
    u64 free_before = universe::free_bytes();
    expect(map_2m(VA & ~0x1FFFFFull, 0x1200000, WRITABLE | NO_EXECUTE) && g_tlb_bytes == tlb::ALL_ADDRESSES && universe::free_bytes() == free_before + 4096,
           "tlb: replacing a page table flushes everything, then frees it");
    expect(unmap(VA & ~0x1FFFFFull, 0x200000), "tlb: a whole huge page unmaps in one step");

    Space* sp = create_space();
    u64 uf = universe::alloc_zeroed(0);
    expect(space_map(sp, 0x400000, uf, 0x1000, WRITABLE | NO_EXECUTE), "tlb: user page mapped");
    u32 c2 = g_tlb_calls;
    space_protect(sp, 0x400000, 0x1000, NO_EXECUTE);
    expect(g_tlb_calls == c2 + 1 && g_tlb_mask == 0, "tlb: a space nobody has loaded shoots down no core");
    destroy_space(sp);
    universe::free(uf, 0);
    universe::free(f, 0);
    g_fake_online = 1;
    (void)hhdm;
}

static void dma_tests(u64 hhdm) {
    constexpr u32 N = 256;
    static dma::Buffer bufs[N];
    Rng rng;
    u32 got = 0;
    bool all_ok = true;
    for (u32 i = 0; i < N; ++i) {
        u64 bytes = 1 + rng() % (3 * 4096);
        bufs[i] = dma::alloc(bytes);
        if (!bufs[i].ok()) { all_ok = false; break; }
        ++got;
    }
    expect(all_ok && got == N, "dma: the table fills to its capacity");
    dma::Buffer extra = dma::alloc(4096);
    expect(!extra.ok(), "dma: allocation beyond the table is refused");

    bool lookups = true;
    for (u32 i = 0; i < N; ++i) {
        const auto& b = bufs[i];
        u64 off = rng() % b.bytes;
        u64 len = 1 + rng() % (b.bytes - off);
        if (dma::phys(static_cast<char*>(b.virt) + off, len) != b.phys + off) lookups = false;
        if (dma::phys(b.virt) != b.phys) lookups = false;
        if (dma::phys(static_cast<char*>(b.virt) + b.bytes - 1, 1) != b.phys + b.bytes - 1) lookups = false;
        if (reinterpret_cast<u64>(b.virt) != hhdm + b.phys) lookups = false;
    }
    expect(lookups, "dma: every offset resolves to the right physical address");

    u32 live = N;
    for (u32 round = 0; round < 3000; ++round) {
        u32 i = rng() % N;
        if (bufs[i].ok()) {
            dma::release(bufs[i]);
            --live;
        } else {
            bufs[i] = dma::alloc(1 + rng() % 8192);
            if (bufs[i].ok()) ++live;
        }
    }
    bool churn = true;
    for (u32 i = 0; i < N; ++i) {
        if (!bufs[i].ok()) continue;
        if (dma::phys(bufs[i].virt, bufs[i].bytes) != bufs[i].phys) churn = false;
    }
    expect(churn, "dma: lookups stay correct through allocate/release churn");

    u64 before = universe::free_bytes();
    for (u32 i = 0; i < N; ++i) if (bufs[i].ok()) dma::release(bufs[i]);
    expect(universe::free_bytes() > before, "dma: releasing returns memory to the buddy allocator");

    auto dies = [](auto fn) {
        pid_t pid = fork();
        if (pid == 0) { fn(); _exit(0); }
        int st = 0;
        waitpid(pid, &st, 0);
        return WIFSIGNALED(st);
    };
    dma::Buffer a = dma::alloc(4096);
    u64 va = reinterpret_cast<u64>(a.virt);
    expect(dies([&] { dma::phys(reinterpret_cast<void*>(va + 4096), 1); }), "dma: a pointer just past a buffer is refused");
    expect(dies([&] { dma::phys(reinterpret_cast<void*>(va + 4000), 200); }), "dma: a range running off a buffer is refused");
    expect(dies([&] { dma::phys(reinterpret_cast<void*>(va - 1), 1); }), "dma: a pointer just below a buffer is refused");
    expect(dies([&] { dma::phys(reinterpret_cast<void*>(8), 1); }), "dma: a foreign pointer is refused");
    expect(dies([&] { dma::Buffer c = a; dma::release(c); dma::Buffer d = a; dma::release(d); }), "dma: releasing a buffer twice is fatal");
    dma::Buffer b = dma::alloc(8192);
    expect(dma::phys(a.virt, a.bytes) == a.phys && dma::phys(b.virt, b.bytes) == b.phys, "dma: neighbouring buffers do not shadow each other");
    dma::release(a);
    dma::release(b);
}

namespace percore {
bool nx_enabled() { return true; }
bool wc_enabled() { return true; }
}

static u64 host_read_u64(u64 va) {
    constellation::Mapping m{};
    if (!constellation::translate(va, &m)) abort();
    return *reinterpret_cast<u64*>(g_hhdm_for_probe + m.phys);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    cpu::g_read_u64 = &host_read_u64;
    void* arena = mmap(nullptr, ARENA, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    u64 hhdm = reinterpret_cast<u64>(arena);
    g_hhdm_for_probe = hhdm;
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
    heap_tests();
    thread_tests();

    constellation::KernelImage image{
        0x200000, 0xffffffff80000000ull,
        0xffffffff80000000ull, 0xffffffff80001000ull,
        0xffffffff80001000ull, 0xffffffff80004000ull,
        0xffffffff80004000ull, 0xffffffff80005000ull,
        0xffffffff80005000ull, 0xffffffff80008000ull
    };
    universe::MemmapEntry vmap[] = {
        { 0x0,       0x9F000,   0 },
        { 0x100000,  0x100000,  0 },
        { 0x200000,  0x8000,    6 },
        { 0x208000,  0x1F8000,  0 },
        { 0x400000,  0x300000,  2 },
        { 0x800000,  0x200000,  1 },
        { 0xC00000,  0x400000,  4 },
        { 0x1000000, 0x1000000, 0 },
        { 0x2200000, 0x400000,  7 },
        { 0x100000000ull, 0x100000, 1 },
    };
    universe::init(vmap, 10, hhdm);
    constellation::init(hhdm, vmap, 10, image);
    expect(constellation::selftest(), "vmm: layout, W^X sections, PAT-preserving split");
    expect(constellation::map_range(hhdm + 0x2200000, 0x2200000, 0x400000, constellation::WRITABLE | constellation::NO_EXECUTE),
           "vmm: explicit range over an unmapped hole");
    constellation::Mapping mp{};
    expect(constellation::translate(hhdm + 0x2201000, &mp) && mp.size == 0x200000, "vmm: hole mapped with a 2 MiB page");
    expect(!constellation::translate(hhdm + 0xC00000, &mp), "vmm: bad memory stays unmapped");
    expect(!constellation::translate(hhdm + 0x100000000ull, &mp), "vmm: reserved memory above 4 GiB stays unmapped");
    expect(!constellation::translate(hhdm + 0x200000, &mp), "vmm: kernel image has no direct-map alias");
    expect(constellation::translate(hhdm + 0x400000, &mp) && !(mp.flags & constellation::NO_CACHE), "vmm: ACPI memory is mapped");
    expect(constellation::translate(0xffffffff80001000ull, &mp) && !(mp.flags & constellation::WRITABLE) && !(mp.flags & constellation::NO_EXECUTE),
           "vmm: .text is read-only and executable");
    expect(constellation::map_range(hhdm + 0x300000, 0x300000, 0x1000, constellation::WRITABLE | constellation::NO_EXECUTE),
           "vmm: remap inside a split page");
    g_hhdm_for_probe = hhdm;
    shootdown_tests(hhdm);
    expect(constellation::smp_selftest(), "vmm: shootdown probe, remap, protect, unmap, space switch");
    dma_tests(hhdm);
    return g_failures ? 1 : 0;
}
