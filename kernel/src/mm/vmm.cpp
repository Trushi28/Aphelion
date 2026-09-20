#include <cosmos/vmm.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/serial.hpp>

namespace constellation {

static u64 g_hhdm = 0;
static u64* g_pml4 = nullptr;

static inline u64 phys_of(void* v) { return reinterpret_cast<u64>(v) - g_hhdm; }
static inline u64* virt_of(u64 p)  { return reinterpret_cast<u64*>(g_hhdm + p); }

static u64* new_table() {
    u64 phys = universe::alloc(0);
    u64* v = virt_of(phys);
    for (int i = 0; i < 512; ++i) v[i] = 0;
    return v;
}

void map_2m(u64 virt_addr, u64 phys_addr, u64 flags) {
    u64 pml4i = (virt_addr >> 39) & 0x1FF;
    u64 pdpti = (virt_addr >> 30) & 0x1FF;
    u64 pdi   = (virt_addr >> 21) & 0x1FF;

    if (!(g_pml4[pml4i] & PRESENT))
        g_pml4[pml4i] = phys_of(new_table()) | PRESENT | WRITABLE;
    u64* pdpt = virt_of(g_pml4[pml4i] & ~0xFFFull);

    if (!(pdpt[pdpti] & PRESENT))
        pdpt[pdpti] = phys_of(new_table()) | PRESENT | WRITABLE;
    u64* pd = virt_of(pdpt[pdpti] & ~0xFFFull);

    pd[pdi] = (phys_addr & ~0x1FFFFFull) | flags | HUGE_PAGE | PRESENT;
}

void map_4k(u64 virt_addr, u64 phys_addr, u64 flags) {
    u64 pml4i = (virt_addr >> 39) & 0x1FF;
    u64 pdpti = (virt_addr >> 30) & 0x1FF;
    u64 pdi   = (virt_addr >> 21) & 0x1FF;
    u64 pti   = (virt_addr >> 12) & 0x1FF;

    if (!(g_pml4[pml4i] & PRESENT))
        g_pml4[pml4i] = phys_of(new_table()) | PRESENT | WRITABLE;
    u64* pdpt = virt_of(g_pml4[pml4i] & ~0xFFFull);

    if (!(pdpt[pdpti] & PRESENT))
        pdpt[pdpti] = phys_of(new_table()) | PRESENT | WRITABLE;
    u64* pd = virt_of(pdpt[pdpti] & ~0xFFFull);

    if (!(pd[pdi] & PRESENT))
        pd[pdi] = phys_of(new_table()) | PRESENT | WRITABLE;
    u64* pt = virt_of(pd[pdi] & ~0xFFFull);

    pt[pti] = (phys_addr & ~0xFFFull) | flags | PRESENT;
}

void init(u64 hhdm_offset, u64 kernel_phys_base, u64 kernel_virt_base,
          u64 kernel_image_size, u64 max_phys_addr) {
    g_hhdm = hhdm_offset;
    g_pml4 = new_table();

    constexpr u64 TWO_MB = 0x200000;
    constexpr u64 FOUR_KB = 0x1000;

    u64 span = (max_phys_addr + TWO_MB - 1) & ~(TWO_MB - 1);
    for (u64 pa = 0; pa < span; pa += TWO_MB)
        map_2m(hhdm_offset + pa, pa, WRITABLE);

    u64 image_span = (kernel_image_size + FOUR_KB - 1) & ~(FOUR_KB - 1);
    for (u64 off = 0; off < image_span; off += FOUR_KB)
        map_4k(kernel_virt_base + off, kernel_phys_base + off, WRITABLE);

    cpu::write_cr3(phys_of(g_pml4));
    serial::printf("[constellation] The Core is live: %lu MiB direct-mapped, CR3=%p\n",
                    span / (1024 * 1024), phys_of(g_pml4));
}

u64 core_pml4_phys() { return phys_of(g_pml4); }

}
