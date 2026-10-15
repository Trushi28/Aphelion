#include <cosmos/vmm.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/serial.hpp>

namespace constellation {

constexpr u64 PHYS_MASK = 0x000FFFFFFFFFF000ull;

static u64 g_hhdm = 0;
static u64* g_pml4 = nullptr;

static inline u64 phys_of(void* v) { return reinterpret_cast<u64>(v) - g_hhdm; }
static inline u64* virt_of(u64 p)  { return reinterpret_cast<u64*>(g_hhdm + p); }

NORETURN static void die(const char* why) {
    serial::printf("[constellation] FATAL: %s\n", why);
    cpu::hang();
}

static u64* new_table() {
    u64 phys = universe::alloc_zeroed(0);
    return phys ? virt_of(phys) : nullptr;
}

static bool split_huge(u64* entry) {
    u64 old = *entry;
    u64 base = old & 0x000FFFFFFFE00000ull;
    u64 flags = (old & 0xFFFull & ~HUGE_PAGE) | (old & NO_EXECUTE);
    u64* pt = new_table();
    if (!pt) return false;
    for (u64 i = 0; i < 512; ++i) pt[i] = (base + i * 0x1000) | flags;
    *entry = phys_of(pt) | PRESENT | WRITABLE;
    return true;
}

static u64* next_level(u64* table, u64 idx) {
    if (!(table[idx] & PRESENT)) {
        u64* t = new_table();
        if (!t) return nullptr;
        table[idx] = phys_of(t) | PRESENT | WRITABLE;
    }
    return virt_of(table[idx] & PHYS_MASK);
}

bool map_2m(u64 virt_addr, u64 phys_addr, u64 flags) {
    if ((virt_addr | phys_addr) & 0x1FFFFF) return false;
    u64* pdpt = next_level(g_pml4, (virt_addr >> 39) & 0x1FF);
    if (!pdpt) return false;
    u64* pd = next_level(pdpt, (virt_addr >> 30) & 0x1FF);
    if (!pd) return false;
    u64 pdi = (virt_addr >> 21) & 0x1FF;
    if ((pd[pdi] & PRESENT) && !(pd[pdi] & HUGE_PAGE)) return false;
    pd[pdi] = phys_addr | flags | HUGE_PAGE | PRESENT;
    cpu::invlpg(virt_addr);
    return true;
}

bool map_4k(u64 virt_addr, u64 phys_addr, u64 flags) {
    if ((virt_addr | phys_addr) & 0xFFF) return false;
    u64* pdpt = next_level(g_pml4, (virt_addr >> 39) & 0x1FF);
    if (!pdpt) return false;
    u64* pd = next_level(pdpt, (virt_addr >> 30) & 0x1FF);
    if (!pd) return false;
    u64 pdi = (virt_addr >> 21) & 0x1FF;
    if (!(pd[pdi] & PRESENT)) {
        u64* t = new_table();
        if (!t) return false;
        pd[pdi] = phys_of(t) | PRESENT | WRITABLE;
    } else if (pd[pdi] & HUGE_PAGE) {
        if (!split_huge(&pd[pdi])) return false;
    }
    u64* pt = virt_of(pd[pdi] & PHYS_MASK);
    pt[(virt_addr >> 12) & 0x1FF] = phys_addr | flags | PRESENT;
    cpu::invlpg(virt_addr);
    return true;
}

void init(u64 hhdm_offset, u64 kernel_phys_base, u64 kernel_virt_base,
          u64 kernel_image_size, u64 max_phys_addr) {
    g_hhdm = hhdm_offset;
    g_pml4 = new_table();
    if (!g_pml4) die("no memory for the top-level page table");

    constexpr u64 TWO_MB = 0x200000;
    constexpr u64 FOUR_KB = 0x1000;

    u64 span = (max_phys_addr + TWO_MB - 1) & ~(TWO_MB - 1);
    for (u64 pa = 0; pa < span; pa += TWO_MB)
        if (!map_2m(hhdm_offset + pa, pa, WRITABLE)) die("could not build the direct map");

    u64 image_span = (kernel_image_size + FOUR_KB - 1) & ~(FOUR_KB - 1);
    for (u64 off = 0; off < image_span; off += FOUR_KB)
        if (!map_4k(kernel_virt_base + off, kernel_phys_base + off, WRITABLE)) die("could not map the kernel image");

    cpu::write_cr3(phys_of(g_pml4));
    serial::printf("[constellation] The Core is live: %lu MiB direct-mapped, CR3=%p\n",
                    span / (1024 * 1024), phys_of(g_pml4));
}

u64 core_pml4_phys() { return phys_of(g_pml4); }

}