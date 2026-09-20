#include <cosmos/pmm.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/cpu.hpp>

namespace universe {

struct FreeNode { FreeNode* next; };

static FreeNode* g_orbits[MAX_ORDER + 1];
static u64 g_hhdm = 0;
static u64 g_free_bytes = 0;
static u64 g_total_bytes = 0;
static u32 g_galaxy_count = 0;

static cpu::Spinlock g_lock;

static inline void* phys_to_virt(u64 p) { return reinterpret_cast<void*>(g_hhdm + p); }
static inline u64 virt_to_phys(void* v) { return reinterpret_cast<u64>(v) - g_hhdm; }

static void orbit_push(int order, u64 phys) {
    auto* n = static_cast<FreeNode*>(phys_to_virt(phys));
    n->next = g_orbits[order];
    g_orbits[order] = n;
}

static bool orbit_remove(int order, u64 phys) {
    void* target = phys_to_virt(phys);
    FreeNode** pp = &g_orbits[order];
    while (*pp) {
        if (*pp == target) { *pp = (*pp)->next; return true; }
        pp = &(*pp)->next;
    }
    return false;
}

static void seed_region(u64 base, u64 length) {
    u64 aligned_base = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    u64 end = (base + length) & ~(PAGE_SIZE - 1);
    if (aligned_base >= end) return;

    ++g_galaxy_count;
    u64 addr = aligned_base;
    u64 remaining = end - aligned_base;
    g_total_bytes += remaining;

    while (remaining >= PAGE_SIZE) {
        int order = 0;
        while (order < MAX_ORDER) {
            u64 next_size = PAGE_SIZE << (order + 1);
            if (next_size > remaining) break;
            if ((addr & (next_size - 1)) != 0) break;
            ++order;
        }
        u64 block_size = PAGE_SIZE << order;
        orbit_push(order, addr);
        g_free_bytes += block_size;
        addr += block_size;
        remaining -= block_size;
    }
}

void init(const MemmapEntry* entries, u64 count, u64 hhdm_offset) {
    g_hhdm = hhdm_offset;
    for (auto& o : g_orbits) o = nullptr;

    for (u64 i = 0; i < count; ++i) {
        if (entries[i].type == 0  )
            seed_region(entries[i].base, entries[i].length);
    }
    serial::printf("[universe] %u usable galax%s seeded, %lu KiB free\n",
                    g_galaxy_count, g_galaxy_count == 1 ? "y" : "ies", g_free_bytes / 1024);
}

void reclaim_bootloader_regions(const MemmapEntry* entries, u64 count) {
    u64 before = g_free_bytes;
    for (u64 i = 0; i < count; ++i) {
        if (entries[i].type == 5  )
            seed_region(entries[i].base, entries[i].length);
    }
    serial::printf("[universe] reclaimed %lu KiB of bootloader-owned memory\n",
                    (g_free_bytes - before) / 1024);
}

u64 alloc(int order) {
    if (order > MAX_ORDER) return 0;
    g_lock.lock();
    int o = order;
    while (o <= MAX_ORDER && !g_orbits[o]) ++o;
    if (o > MAX_ORDER) { g_lock.unlock(); return 0; }

    FreeNode* n = g_orbits[o];
    g_orbits[o] = n->next;
    u64 addr = virt_to_phys(n);

    while (o > order) {
        --o;
        orbit_push(o, addr + (PAGE_SIZE << o));
    }
    g_free_bytes -= (PAGE_SIZE << order);
    g_lock.unlock();
    return addr;
}

void free(u64 phys_addr, int order) {
    g_lock.lock();
    while (order < MAX_ORDER) {
        u64 buddy = phys_addr ^ (PAGE_SIZE << order);
        if (!orbit_remove(order, buddy)) break;
        phys_addr = phys_addr < buddy ? phys_addr : buddy;
        ++order;
    }
    orbit_push(order, phys_addr);
    g_free_bytes += (PAGE_SIZE << order);
    g_lock.unlock();
}

u64 free_bytes()  { return g_free_bytes; }
u64 total_bytes() { return g_total_bytes; }
u32 galaxy_count() { return g_galaxy_count; }

}
