#include <cosmos/vmm.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/serial.hpp>

namespace constellation {

constexpr u64 PHYS_MASK = 0x000FFFFFFFFFF000ull;

static u64 g_hhdm = 0;
static u64* g_pml4 = nullptr;
static volatile bool g_sealed = false;

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
    u64 flags = (old & 0xF7Full) | (old & NO_EXECUTE);
    if (old & PAT_2M) flags |= PAT_4K;
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
    if (g_sealed) die("map_2m after the secondary cores were started");
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
    if (g_sealed) die("map_4k after the secondary cores were started");
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

bool translate(u64 virt_addr, Mapping* out) {
    u64* table = g_pml4;
    u64 writable = WRITABLE;
    u64 no_exec = 0;
    for (int shift = 39; shift >= 12; shift -= 9) {
        u64 e = table[(virt_addr >> shift) & 0x1FF];
        if (!(e & PRESENT)) return false;
        writable &= e;
        no_exec |= e & NO_EXECUTE;
        bool leaf = shift == 12 || (shift != 39 && (e & HUGE_PAGE));
        if (!leaf) { table = virt_of(e & PHYS_MASK); continue; }
        u64 size = 1ull << shift;
        u64 pat_bit = shift == 12 ? PAT_4K : PAT_2M;
        out->phys = (e & PHYS_MASK & ~(size - 1)) | (virt_addr & (size - 1));
        out->size = size;
        out->flags = (e & (PRESENT | WRITE_THROUGH | NO_CACHE)) | writable | no_exec;
        out->pat = (e & pat_bit) != 0;
        return true;
    }
    return false;
}

void seal() { g_sealed = true; }

bool selftest() {
    constexpr u64 WINDOW = 0xFFFFE00000000000ull;
    constexpr u64 UNUSED_VA = 0xFFFFF00000000000ull;
    constexpr u64 PHYS_A = 0x40000000ull;
    constexpr u64 PHYS_B = 0x80005000ull;
    Mapping m{};
    bool ok = true;

    ok = ok && !map_2m(WINDOW + 0x1000, PHYS_A, WRITABLE);
    ok = ok && !map_2m(WINDOW, PHYS_A + 0x1000, WRITABLE);
    ok = ok && !map_4k(WINDOW + 1, PHYS_B, WRITABLE);
    ok = ok && !map_4k(WINDOW, PHYS_B + 1, WRITABLE);
    ok = ok && !translate(UNUSED_VA, &m);

    ok = ok && map_2m(WINDOW, PHYS_A, WRITABLE | NO_EXECUTE | NO_CACHE | PAT_2M);
    ok = ok && translate(WINDOW + 0x1234, &m);
    ok = ok && m.size == 0x200000 && m.phys == PHYS_A + 0x1234 && m.pat;
    ok = ok && (m.flags & WRITABLE) && (m.flags & NO_EXECUTE) && (m.flags & NO_CACHE);

    ok = ok && map_4k(WINDOW + 0x5000, PHYS_B, WRITABLE);
    ok = ok && translate(WINDOW + 0x5008, &m);
    ok = ok && m.size == 0x1000 && m.phys == PHYS_B + 8 && !m.pat;
    ok = ok && (m.flags & WRITABLE) && !(m.flags & NO_EXECUTE) && !(m.flags & NO_CACHE);

    constexpr u64 PROBES[3] = { 0x0ull, 0x3000ull, 0x1FF000ull };
    for (u64 off : PROBES) {
        ok = ok && translate(WINDOW + off + 0x10, &m);
        ok = ok && m.size == 0x1000 && m.phys == PHYS_A + off + 0x10 && m.pat;
        ok = ok && (m.flags & WRITABLE) && (m.flags & NO_EXECUTE) && (m.flags & NO_CACHE);
    }

    ok = ok && translate(g_hhdm + 0x200000, &m);
    ok = ok && m.phys == 0x200000 && (m.flags & PRESENT);
    return ok;
}

u64 core_pml4_phys() { return phys_of(g_pml4); }

}