#include <cosmos/vmm.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/percore.hpp>

namespace constellation {

constexpr u64 PHYS_MASK = 0x000FFFFFFFFFF000ull;

static u64 g_hhdm = 0;
static u64* g_pml4 = nullptr;
static volatile bool g_sealed = false;

struct Range { u64 lo, hi; };

constexpr u64 TWO_MB = 0x200000;
constexpr u64 FOUR_KB = 0x1000;
constexpr u64 FOUR_GB = 1ull << 32;
constexpr u64 LOW_WINDOW = 0x100000;
constexpr u32 MAX_ENTRIES = 512;
constexpr u32 MAX_RANGES = 128;
constexpr u64 HHDM_FLAGS = WRITABLE | NO_EXECUTE;

constexpr u64 MEM_USABLE = 0;
constexpr u64 MEM_RESERVED = 1;
constexpr u64 MEM_ACPI_RECLAIMABLE = 2;
constexpr u64 MEM_ACPI_NVS = 3;
constexpr u64 MEM_BOOTLOADER_RECLAIMABLE = 5;

static Range g_scratch[MAX_ENTRIES + 1];
static Range g_ranges[MAX_RANGES];
static u32 g_range_count = 0;
static u32 g_range_overflow = 0;
static KernelImage g_image{};

static inline u64 adjust(u64 flags) {
    return percore::nx_enabled() ? flags : (flags & ~NO_EXECUTE);
}

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
    flags = adjust(flags);
    if ((virt_addr | phys_addr) & 0x1FFFFF) return false;
    u64* pdpt = next_level(g_pml4, (virt_addr >> 39) & 0x1FF);
    if (!pdpt) return false;
    u64* pd = next_level(pdpt, (virt_addr >> 30) & 0x1FF);
    if (!pd) return false;
    u64 pdi = (virt_addr >> 21) & 0x1FF;
    bool replaced_table = (pd[pdi] & PRESENT) && !(pd[pdi] & HUGE_PAGE);
    u64 old_table = pd[pdi] & PHYS_MASK;
    pd[pdi] = phys_addr | flags | HUGE_PAGE | PRESENT;
    if (replaced_table) {
        cpu::write_cr3(cpu::read_cr3());
        universe::free(old_table, 0);
    }
    cpu::invlpg(virt_addr);
    return true;
}

bool map_4k(u64 virt_addr, u64 phys_addr, u64 flags) {
    if (g_sealed) die("map_4k after the secondary cores were started");
    flags = adjust(flags);
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

bool map_range(u64 virt_addr, u64 phys_addr, u64 bytes, u64 flags) {
    if ((virt_addr | phys_addr | bytes) & (FOUR_KB - 1)) return false;
    while (bytes) {
        bool big = !((virt_addr | phys_addr) & (TWO_MB - 1)) && bytes >= TWO_MB;
        u64 step = big ? TWO_MB : FOUR_KB;
        if (!(big ? map_2m(virt_addr, phys_addr, flags) : map_4k(virt_addr, phys_addr, flags))) return false;
        virt_addr += step;
        phys_addr += step;
        bytes -= step;
    }
    return true;
}

static bool hhdm_wants(const universe::MemmapEntry& e) {
    switch (e.type) {
        case MEM_USABLE:
        case MEM_ACPI_RECLAIMABLE:
        case MEM_ACPI_NVS:
        case MEM_BOOTLOADER_RECLAIMABLE:
            return true;
        case MEM_RESERVED:
            return e.base < FOUR_GB;
        default:
            return false;
    }
}

static void record_range(u64 lo, u64 hi) {
    if (g_range_count < MAX_RANGES) g_ranges[g_range_count++] = { lo, hi };
    else ++g_range_overflow;
}

static u64 build_direct_map(u64 hhdm_offset, const universe::MemmapEntry* entries, u64 count) {
    if (count > MAX_ENTRIES) die("memory map has more entries than the direct map builder supports");
    u64 n = 0;
    g_scratch[n++] = { 0, LOW_WINDOW };
    for (u64 i = 0; i < count; ++i) {
        if (!hhdm_wants(entries[i])) continue;
        u64 lo = entries[i].base & ~(FOUR_KB - 1);
        u64 hi = (entries[i].base + entries[i].length + FOUR_KB - 1) & ~(FOUR_KB - 1);
        if (hi > lo) g_scratch[n++] = { lo, hi };
    }
    for (u64 i = 1; i < n; ++i) {
        Range key = g_scratch[i];
        u64 j = i;
        while (j > 0 && g_scratch[j - 1].lo > key.lo) { g_scratch[j] = g_scratch[j - 1]; --j; }
        g_scratch[j] = key;
    }

    u64 mapped = 0;
    Range cur = g_scratch[0];
    for (u64 i = 1; i <= n; ++i) {
        bool last = i == n;
        if (!last && g_scratch[i].lo <= cur.hi) {
            if (g_scratch[i].hi > cur.hi) cur.hi = g_scratch[i].hi;
            continue;
        }
        if (!map_range(hhdm_offset + cur.lo, cur.lo, cur.hi - cur.lo, HHDM_FLAGS)) die("could not build the direct map");
        record_range(cur.lo, cur.hi);
        mapped += cur.hi - cur.lo;
        if (!last) cur = g_scratch[i];
    }
    return mapped;
}

static void map_image_section(const char* name, u64 start, u64 end, u64 flags, const char* perms) {
    if (end <= start) return;
    u64 phys = g_image.phys_base + (start - g_image.virt_base);
    if (!map_range(start, phys, end - start, flags)) die("could not map the kernel image");
    serial::printf("[constellation] image %s %p - %p %s\n", name, start, end, perms);
}

void init(u64 hhdm_offset, const universe::MemmapEntry* entries, u64 count, const KernelImage& image) {
    g_hhdm = hhdm_offset;
    g_image = image;
    g_pml4 = new_table();
    if (!g_pml4) die("no memory for the top-level page table");

    u64 mapped = build_direct_map(hhdm_offset, entries, count);

    map_image_section(".requests", image.requests_start, image.requests_end, WRITABLE | NO_EXECUTE, "RW NX");
    map_image_section(".text", image.text_start, image.text_end, 0, "R  X");
    map_image_section(".rodata", image.rodata_start, image.rodata_end, NO_EXECUTE, "R  NX");
    map_image_section(".data/.bss", image.data_start, image.data_end, WRITABLE | NO_EXECUTE, "RW NX");

    for (u32 i = 0; i < g_range_count; ++i)
        serial::printf("[constellation] direct map %p - %p RW NX (%lu KiB)\n",
                        g_ranges[i].lo, g_ranges[i].hi, (g_ranges[i].hi - g_ranges[i].lo) / 1024);
    if (g_range_overflow) serial::printf("[constellation] %u further direct-map ranges not listed\n", g_range_overflow);

    cpu::write_cr3(phys_of(g_pml4));
    serial::printf("[constellation] The Core is live: %lu MiB direct-mapped in %u range(s), NX %s, CR3=%p\n",
                    mapped / (1024 * 1024), g_range_count + g_range_overflow,
                    percore::nx_enabled() ? "on" : "off", phys_of(g_pml4));
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

bool in_stack_guard(u64 addr) {
    if (addr < STACK_REGION_BASE || addr >= STACK_REGION_BASE + STACK_SLOT_BYTES * STACK_SLOTS) return false;
    u64 off = (addr - STACK_REGION_BASE) % STACK_SLOT_BYTES;
    return off < STACK_GUARD_BYTES || off >= STACK_GUARD_BYTES + STACK_BYTES;
}

static bool page_is(u64 virt, u64 phys, bool writable, bool executable) {
    Mapping m{};
    if (!translate(virt, &m) || m.phys != phys || !(m.flags & PRESENT)) return false;
    if (((m.flags & WRITABLE) != 0) != writable) return false;
    if (percore::nx_enabled() && ((m.flags & NO_EXECUTE) == 0) != executable) return false;
    return true;
}

static bool section_is(u64 start, u64 end, bool writable, bool executable) {
    if (end <= start) return true;
    u64 phys = g_image.phys_base + (start - g_image.virt_base);
    u64 last = end - FOUR_KB;
    return page_is(start, phys, writable, executable) &&
           page_is(last, phys + (last - start), writable, executable);
}

static bool verify_layout() {
    bool ok = true;
    for (u32 i = 0; i < g_range_count; ++i) {
        ok = ok && page_is(g_hhdm + g_ranges[i].lo, g_ranges[i].lo, true, false);
        ok = ok && page_is(g_hhdm + g_ranges[i].hi - FOUR_KB, g_ranges[i].hi - FOUR_KB, true, false);
    }
    Mapping m{};
    ok = ok && !translate(g_hhdm + g_image.phys_base, &m);
    ok = ok && section_is(g_image.requests_start, g_image.requests_end, true, false);
    ok = ok && section_is(g_image.text_start, g_image.text_end, false, true);
    ok = ok && section_is(g_image.rodata_start, g_image.rodata_end, false, false);
    ok = ok && section_is(g_image.data_start, g_image.data_end, true, false);
    return ok;
}

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
    ok = ok && (m.flags & WRITABLE) && ((m.flags & NO_EXECUTE) || !percore::nx_enabled()) && (m.flags & NO_CACHE);

    ok = ok && map_4k(WINDOW + 0x5000, PHYS_B, WRITABLE);
    ok = ok && translate(WINDOW + 0x5008, &m);
    ok = ok && m.size == 0x1000 && m.phys == PHYS_B + 8 && !m.pat;
    ok = ok && (m.flags & WRITABLE) && !(m.flags & NO_EXECUTE) && !(m.flags & NO_CACHE);

    constexpr u64 PROBES[3] = { 0x0ull, 0x3000ull, 0x1FF000ull };
    for (u64 off : PROBES) {
        ok = ok && translate(WINDOW + off + 0x10, &m);
        ok = ok && m.size == 0x1000 && m.phys == PHYS_A + off + 0x10 && m.pat;
        ok = ok && (m.flags & WRITABLE) && ((m.flags & NO_EXECUTE) || !percore::nx_enabled()) && (m.flags & NO_CACHE);
    }

    ok = ok && verify_layout();
    return ok;
}

u64 core_pml4_phys() { return phys_of(g_pml4); }

}