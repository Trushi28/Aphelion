#include <cosmos/pmm.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/cpu.hpp>

namespace universe {

struct FreeNode { FreeNode* next; FreeNode* prev; };

constexpr u64 MEM_USABLE = 0;
constexpr u64 MEM_BOOTLOADER_RECLAIMABLE = 5;
constexpr u64 LOW_RESERVE = 0x100000;

enum Zone : int { ZONE_HIGH = 0, ZONE_DMA32 = 1, ZONES = 2 };

static FreeNode* g_orbits[ZONES][MAX_ORDER + 1];
static u64 g_hhdm = 0;
static u64 g_free_bytes = 0;
static u64 g_total_bytes = 0;
static u32 g_galaxy_count = 0;
static u64 g_pages = 0;
static u64* g_owned = nullptr;
static u64* g_free = nullptr;
static u8* g_tag = nullptr;

static cpu::Spinlock g_lock;

struct LockGuard {
    u64 flags;
    LockGuard() : flags(cpu::irq_save()) { g_lock.lock(); }
    ~LockGuard() { g_lock.unlock(); cpu::irq_restore(flags); }
};

NORETURN static void fatal(const char* what, u64 phys, int order) {
    serial::printf("[universe] FATAL: %s (phys %p, order %d)\n", what, phys, order);
    cpu::hang();
}

static inline bool bit_get(const u64* m, u64 i) { return (m[i >> 6] >> (i & 63)) & 1; }
static inline void bit_put(u64* m, u64 i, bool v) {
    if (v) m[i >> 6] |= 1ull << (i & 63);
    else m[i >> 6] &= ~(1ull << (i & 63));
}
static void bits_fill(u64* m, u64 first, u64 n, bool v) {
    while (n && (first & 63)) { bit_put(m, first++, v); --n; }
    while (n >= 64) { m[first >> 6] = v ? ~0ull : 0ull; first += 64; n -= 64; }
    while (n) { bit_put(m, first++, v); --n; }
}
static bool bits_all(const u64* m, u64 first, u64 n, bool v) {
    while (n && (first & 63)) { if (bit_get(m, first++) != v) return false; --n; }
    u64 want = v ? ~0ull : 0ull;
    while (n >= 64) { if (m[first >> 6] != want) return false; first += 64; n -= 64; }
    while (n) { if (bit_get(m, first++) != v) return false; --n; }
    return true;
}

static inline void* phys_to_virt(u64 p) { return reinterpret_cast<void*>(g_hhdm + p); }
static inline u64 virt_to_phys(void* v) { return reinterpret_cast<u64>(v) - g_hhdm; }
static inline int zone_of(u64 phys) { return phys < DMA32_LIMIT ? ZONE_DMA32 : ZONE_HIGH; }

void* to_virt(u64 phys) { return phys_to_virt(phys); }
u64 to_phys(const void* virt) { return reinterpret_cast<u64>(virt) - g_hhdm; }
u64 page_count() { return g_pages; }

static void orbit_push(int order, u64 phys) {
    auto* n = static_cast<FreeNode*>(phys_to_virt(phys));
    FreeNode*& head = g_orbits[zone_of(phys)][order];
    n->prev = nullptr;
    n->next = head;
    if (head) head->prev = n;
    head = n;
    g_tag[phys >> 12] = static_cast<u8>(order + 1);
}

static void orbit_unlink(int order, u64 phys) {
    if (g_tag[phys >> 12] != order + 1) fatal("free list corrupt: block is not a free head of this order", phys, order);
    auto* n = static_cast<FreeNode*>(phys_to_virt(phys));
    if (n->prev) n->prev->next = n->next;
    else g_orbits[zone_of(phys)][order] = n->next;
    if (n->next) n->next->prev = n->prev;
    g_tag[phys >> 12] = 0;
}

static void insert_coalescing(u64 phys, int order) {
    while (order < MAX_ORDER) {
        u64 buddy = phys ^ (PAGE_SIZE << order);
        if ((buddy >> 12) >= g_pages || g_tag[buddy >> 12] != order + 1) break;
        orbit_unlink(order, buddy);
        phys = phys < buddy ? phys : buddy;
        ++order;
    }
    orbit_push(order, phys);
}

static void seed_region(u64 base, u64 length) {
    u64 end = base + length;
    if (base < LOW_RESERVE) base = LOW_RESERVE;
    base = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    end &= ~(PAGE_SIZE - 1);
    if ((end >> 12) > g_pages) end = g_pages << 12;
    if (base >= end) return;

    ++g_galaxy_count;
    u64 addr = base;
    u64 remaining = end - base;
    g_total_bytes += remaining;
    bits_fill(g_owned, base >> 12, remaining >> 12, true);
    bits_fill(g_free, base >> 12, remaining >> 12, true);

    while (remaining >= PAGE_SIZE) {
        int order = 0;
        while (order < MAX_ORDER) {
            u64 next_size = PAGE_SIZE << (order + 1);
            if (next_size > remaining) break;
            if ((addr & (next_size - 1)) != 0) break;
            ++order;
        }
        u64 block_size = PAGE_SIZE << order;
        insert_coalescing(addr, order);
        g_free_bytes += block_size;
        addr += block_size;
        remaining -= block_size;
    }
}

void init(const MemmapEntry* entries, u64 count, u64 hhdm_offset) {
    g_hhdm = hhdm_offset;
    for (auto& z : g_orbits) for (auto& o : z) o = nullptr;

    u64 top = 0;
    for (u64 i = 0; i < count; ++i) {
        if (entries[i].type != MEM_USABLE && entries[i].type != MEM_BOOTLOADER_RECLAIMABLE) continue;
        u64 e = entries[i].base + entries[i].length;
        if (e > top) top = e;
    }
    g_pages = top >> 12;
    u64 words = (g_pages + 63) / 64;
    u64 map_bytes = ((words * 16 + g_pages + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;

    u64 carve = 0, carve_idx = ~0ull;
    for (u64 i = 0; i < count && carve_idx == ~0ull; ++i) {
        if (entries[i].type != MEM_USABLE) continue;
        u64 lo = entries[i].base < LOW_RESERVE ? LOW_RESERVE : entries[i].base;
        lo = (lo + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        u64 hi = (entries[i].base + entries[i].length) & ~(PAGE_SIZE - 1);
        if (hi > lo && hi - lo > map_bytes) { carve = lo; carve_idx = i; }
    }
    if (carve_idx == ~0ull) {
        serial::writeln("[universe] FATAL: no usable region large enough for the page-state maps");
        cpu::hang();
    }
    g_owned = static_cast<u64*>(phys_to_virt(carve));
    g_free = g_owned + words;
    g_tag = reinterpret_cast<u8*>(g_free + words);
    __builtin_memset(g_owned, 0, map_bytes);

    for (u64 i = 0; i < count; ++i) {
        if (entries[i].type != MEM_USABLE) continue;
        u64 base = entries[i].base, end = base + entries[i].length;
        if (i == carve_idx) base = carve + map_bytes;
        if (end > base) seed_region(base, end - base);
    }
    serial::printf("[universe] %u usable galax%s seeded, %lu KiB free, page-state maps %lu KiB\n",
                    g_galaxy_count, g_galaxy_count == 1 ? "y" : "ies", g_free_bytes / 1024, map_bytes / 1024);
}

static u64 popcount64(u64 v) {
    u64 n = 0;
    while (v) { v &= v - 1; ++n; }
    return n;
}

static bool check_invariants() {
    u64 listed = 0, bytes = 0;
    for (int z = 0; z < ZONES; ++z) {
        for (int o = 0; o <= MAX_ORDER; ++o) {
            u64 size = PAGE_SIZE << o;
            FreeNode* prev = nullptr;
            for (FreeNode* n = g_orbits[z][o]; n; prev = n, n = n->next) {
                u64 phys = virt_to_phys(n);
                if (n->prev != prev) return false;
                if ((phys & (size - 1)) || (phys >> 12) + (1ull << o) > g_pages) return false;
                if (zone_of(phys) != z) return false;
                if (g_tag[phys >> 12] != o + 1) return false;
                if (!bits_all(g_free, phys >> 12, 1ull << o, true)) return false;
                if (o < MAX_ORDER) {
                    u64 buddy = phys ^ size;
                    if ((buddy >> 12) < g_pages && g_tag[buddy >> 12] == o + 1) return false;
                }
                bytes += size;
                if (++listed > g_pages) return false;
            }
        }
    }
    u64 tags = 0;
    for (u64 i = 0; i < g_pages; ++i) if (g_tag[i]) ++tags;
    u64 free_pages = 0;
    for (u64 w = 0; w < (g_pages + 63) / 64; ++w) free_pages += popcount64(g_free[w]);
    return tags == listed && bytes == g_free_bytes && free_pages == (g_free_bytes >> 12);
}

void reclaim_bootloader_regions(const MemmapEntry* entries, u64 count) {
    u64 reclaimed;
    bool consistent;
    {
        LockGuard g;
        u64 before = g_free_bytes;
        for (u64 i = 0; i < count; ++i)
            if (entries[i].type == MEM_BOOTLOADER_RECLAIMABLE) seed_region(entries[i].base, entries[i].length);
        reclaimed = g_free_bytes - before;
        consistent = check_invariants();
    }
    serial::printf("[universe] reclaimed %lu KiB of bootloader-owned memory, free lists %s\n",
                    reclaimed / 1024, consistent ? "consistent" : "CORRUPT (MISMATCH)");
}

static u64 alloc_from(int zone, int order) {
    int o = order;
    while (o <= MAX_ORDER && !g_orbits[zone][o]) ++o;
    if (o > MAX_ORDER) return 0;
    u64 addr = virt_to_phys(g_orbits[zone][o]);
    orbit_unlink(o, addr);
    while (o > order) {
        --o;
        orbit_push(o, addr + (PAGE_SIZE << o));
    }
    return addr;
}

static u64 take(int order, bool dma32_only) {
    if (order < 0 || order > MAX_ORDER) return 0;
    LockGuard g;
    u64 addr = 0;
    if (!dma32_only) addr = alloc_from(ZONE_HIGH, order);
    if (!addr) addr = alloc_from(ZONE_DMA32, order);
    if (addr) {
        bits_fill(g_free, addr >> 12, 1ull << order, false);
        g_free_bytes -= PAGE_SIZE << order;
    }
    return addr;
}

u64 alloc(int order) { return take(order, false); }
u64 alloc_dma32(int order) { return take(order, true); }

u64 alloc_zeroed(int order, bool dma32) {
    u64 p = take(order, dma32);
    if (p) __builtin_memset(phys_to_virt(p), 0, PAGE_SIZE << order);
    return p;
}

void free(u64 phys, int order) {
    if (order < 0 || order > MAX_ORDER) fatal("free with a bad order", phys, order);
    u64 size = PAGE_SIZE << order;
    u64 pages = 1ull << order;
    if ((phys & (size - 1)) || (phys >> 12) + pages > g_pages)
        fatal("free of a misaligned or out-of-range block", phys, order);

    LockGuard g;
    if (!bits_all(g_owned, phys >> 12, pages, true)) fatal("free of memory the PMM never owned", phys, order);
    if (!bits_all(g_free, phys >> 12, pages, false)) fatal("double free", phys, order);

    bits_fill(g_free, phys >> 12, pages, true);
    g_free_bytes += size;
    insert_coalescing(phys, order);
}

u64 free_bytes() { return g_free_bytes; }
u64 total_bytes() { return g_total_bytes; }
u32 galaxy_count() { return g_galaxy_count; }

static bool locked_check() {
    LockGuard g;
    return check_invariants();
}

static void count_blocks(u64* out) {
    LockGuard g;
    for (int o = 0; o <= MAX_ORDER; ++o) {
        out[o] = 0;
        for (int z = 0; z < ZONES; ++z)
            for (FreeNode* n = g_orbits[z][o]; n; n = n->next) ++out[o];
    }
}

bool selftest() {
    static u64 held[512];
    u64 shape_before[MAX_ORDER + 1];
    u64 shape_after[MAX_ORDER + 1];
    u64 before = free_bytes();
    bool ok = locked_check();
    count_blocks(shape_before);

    for (int o = 0; o <= 6 && ok; ++o) {
        u64 size = PAGE_SIZE << o;
        u64 p = alloc_zeroed(o, true);
        if (!p) { ok = false; break; }
        if ((p & (size - 1)) || p < LOW_RESERVE || p + size > DMA32_LIMIT) ok = false;
        const u8* b = static_cast<const u8*>(phys_to_virt(p));
        for (u64 i = 0; ok && i < size; ++i) if (b[i]) ok = false;
        free(p, o);
    }

    u64 n = 0;
    for (; n < 512; ++n) { held[n] = alloc(0); if (!held[n]) break; }
    ok = ok && n == 512;
    for (u64 i = 0; i < n; ++i) free(held[n == 512 ? (i * 211) % 512 : i], 0);

    u64 big = alloc(7);
    ok = ok && big != 0;
    if (big) free(big, 7);

    count_blocks(shape_after);
    for (int o = 0; o <= MAX_ORDER; ++o) if (shape_before[o] != shape_after[o]) ok = false;
    return ok && locked_check() && free_bytes() == before;
}

}