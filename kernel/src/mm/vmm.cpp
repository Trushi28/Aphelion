#include <cosmos/vmm.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/percore.hpp>
#include <cosmos/tlb.hpp>

namespace constellation {

constexpr u64 PHYS_MASK = 0x000FFFFFFFFFF000ull;

struct Space {
    u64* root;
    u64 root_phys;
    volatile u64 cpumask;
    bool in_use;
};

constexpr u32 MAX_SPACES = 64;
constexpr u32 KERNEL_SLOT_FIRST = 256;

static u64 g_hhdm = 0;
static Space g_core{};
static Space g_spaces[MAX_SPACES];
static Space* g_active[tlb::MAX_CORES];
static volatile int g_lock_word = 0;

struct Range { u64 lo, hi; };

constexpr u64 TWO_MB = 0x200000;
constexpr u64 FOUR_KB = 0x1000;
constexpr u64 FOUR_GB = 1ull << 32;
constexpr u64 LOW_WINDOW = 0x100000;
constexpr u32 MAX_ENTRIES = 512;
constexpr u32 MAX_RANGES = 128;
constexpr u64 HHDM_FLAGS = WRITABLE | NO_EXECUTE;
constexpr u64 PERM_BITS = WRITABLE | NO_EXECUTE;
constexpr u64 CACHE_BITS = WRITE_THROUGH | NO_CACHE;
constexpr u64 FIRMWARE_FLAGS = NO_CACHE | NO_EXECUTE;
constexpr u64 FIRMWARE_MAX_BYTES = 64ull << 20;
constexpr u32 FIRMWARE_CACHE = 64;

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

struct FirmwareMap { u64 phys, bytes, virt; };
static FirmwareMap g_fw[FIRMWARE_CACHE];
static u32 g_fw_count = 0;
static u32 g_fw_pages = 0;

struct Guard {
    u64 flags;
    Guard() : flags(cpu::irq_save()) {
        for (;;) {
            if (!__atomic_exchange_n(&g_lock_word, 1, __ATOMIC_ACQUIRE)) break;
            while (__atomic_load_n(&g_lock_word, __ATOMIC_RELAXED)) {
                tlb::poll();
                asm volatile("pause" ::: "memory");
            }
        }
    }
    ~Guard() {
        __atomic_store_n(&g_lock_word, 0, __ATOMIC_RELEASE);
        cpu::irq_restore(flags);
    }
};

static inline u64 adjust(u64 flags) {
    return percore::nx_enabled() ? flags : (flags & ~NO_EXECUTE);
}

static inline bool is_user(const Space& s) { return &s != &g_core; }
static inline u64 table_flags(const Space& s) { return PRESENT | WRITABLE | (is_user(s) ? USER : 0); }
static inline u64 leaf_extra(const Space& s) { return is_user(s) ? USER : 0; }
static inline u64 flush_mask(const Space& s) { return is_user(s) ? s.cpumask : tlb::online_mask(); }
static inline void shoot(const Space& s, u64 virt, u64 bytes) { tlb::flush(flush_mask(s), virt, bytes); }

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

static bool split_huge(const Space& s, u64* entry) {
    u64 old = *entry;
    u64 base = old & 0x000FFFFFFFE00000ull;
    u64 flags = (old & 0xF7Full) | (old & NO_EXECUTE);
    if (old & PAT_2M) flags |= PAT_4K;
    u64* pt = new_table();
    if (!pt) return false;
    for (u64 i = 0; i < 512; ++i) pt[i] = (base + i * 0x1000) | flags;
    *entry = phys_of(pt) | table_flags(s);
    return true;
}

static u64* child(const Space& s, u64* table, u64 idx, bool create) {
    u64 e = table[idx];
    if (e & PRESENT) return virt_of(e & PHYS_MASK);
    if (!create) return nullptr;
    u64* t = new_table();
    if (!t) return nullptr;
    table[idx] = phys_of(t) | table_flags(s);
    return t;
}

static u64* pd_of(const Space& s, u64 virt, bool create) {
    u64* pdpt = child(s, s.root, (virt >> 39) & 0x1FF, create);
    if (!pdpt) return nullptr;
    if (pdpt[(virt >> 30) & 0x1FF] & HUGE_PAGE) return nullptr;
    return child(s, pdpt, (virt >> 30) & 0x1FF, create);
}

static u64* pt_of(const Space& s, u64 virt, bool create, bool split) {
    u64* pd = pd_of(s, virt, create);
    if (!pd) return nullptr;
    u64* e = &pd[(virt >> 21) & 0x1FF];
    if (!(*e & PRESENT)) {
        if (!create) return nullptr;
        u64* t = new_table();
        if (!t) return nullptr;
        *e = phys_of(t) | table_flags(s);
    } else if (*e & HUGE_PAGE) {
        if (!split || !split_huge(s, e)) return nullptr;
    }
    return virt_of(*e & PHYS_MASK);
}

static bool leaf_of(const Space& s, u64 virt, u64** entry, u64* size) {
    u64* pd = pd_of(s, virt, false);
    if (!pd) return false;
    u64* pde = &pd[(virt >> 21) & 0x1FF];
    if (!(*pde & PRESENT)) return false;
    if (*pde & HUGE_PAGE) { *entry = pde; *size = TWO_MB; return true; }
    u64* pte = &virt_of(*pde & PHYS_MASK)[(virt >> 12) & 0x1FF];
    if (!(*pte & PRESENT)) return false;
    *entry = pte;
    *size = FOUR_KB;
    return true;
}

static bool map_2m_in(Space& s, u64 virt, u64 phys, u64 flags) {
    flags = adjust(flags) | leaf_extra(s);
    if ((virt | phys) & (TWO_MB - 1)) return false;
    u64* pd = pd_of(s, virt, true);
    if (!pd) return false;
    u64* e = &pd[(virt >> 21) & 0x1FF];
    u64 old = *e;
    *e = phys | flags | HUGE_PAGE | PRESENT;
    if (old & PRESENT) {
        bool was_table = !(old & HUGE_PAGE);
        shoot(s, virt, was_table ? tlb::ALL_ADDRESSES : TWO_MB);
        if (was_table) universe::free(old & PHYS_MASK, 0);
    } else {
        cpu::invlpg(virt);
    }
    return true;
}

static bool map_4k_in(Space& s, u64 virt, u64 phys, u64 flags) {
    flags = adjust(flags) | leaf_extra(s);
    if ((virt | phys) & (FOUR_KB - 1)) return false;
    u64* pt = pt_of(s, virt, true, true);
    if (!pt) return false;
    u64* e = &pt[(virt >> 12) & 0x1FF];
    u64 old = *e;
    *e = phys | flags | PRESENT;
    if (old & PRESENT) shoot(s, virt, FOUR_KB);
    else cpu::invlpg(virt);
    return true;
}

static bool unmap_in(Space& s, u64 virt, u64 bytes) {
    if (!bytes || ((virt | bytes) & (FOUR_KB - 1))) return false;
    u64 end = virt + bytes;
    if (end < virt) return false;
    u64* e = nullptr;
    u64 size = 0;
    for (u64 a = virt; a < end; a += FOUR_KB)
        if (!leaf_of(s, a, &e, &size)) return false;
    bool ok = true;
    for (u64 a = virt; a < end;) {
        leaf_of(s, a, &e, &size);
        if (size == TWO_MB) {
            if (!(a & (TWO_MB - 1)) && end - a >= TWO_MB) { *e = 0; a += TWO_MB; continue; }
            if (!pt_of(s, a, false, true)) { ok = false; break; }
            continue;
        }
        *e = 0;
        a += FOUR_KB;
    }
    shoot(s, virt, bytes);
    return ok;
}

static bool protect_in(Space& s, u64 virt, u64 bytes, u64 flags) {
    if (!bytes || ((virt | bytes) & (FOUR_KB - 1))) return false;
    if (flags & ~PERM_BITS) return false;
    u64 end = virt + bytes;
    if (end < virt) return false;
    u64 bits = adjust(flags);
    u64* e = nullptr;
    u64 size = 0;
    for (u64 a = virt; a < end; a += FOUR_KB)
        if (!leaf_of(s, a, &e, &size)) return false;
    bool ok = true;
    for (u64 a = virt; a < end;) {
        leaf_of(s, a, &e, &size);
        if (size == TWO_MB) {
            if (!(a & (TWO_MB - 1)) && end - a >= TWO_MB) { *e = (*e & ~PERM_BITS) | bits; a += TWO_MB; continue; }
            if (!pt_of(s, a, false, true)) { ok = false; break; }
            continue;
        }
        *e = (*e & ~PERM_BITS) | bits;
        a += FOUR_KB;
    }
    shoot(s, virt, bytes);
    return ok;
}

static bool map_range_in(Space& s, u64 virt, u64 phys, u64 bytes, u64 flags) {
    if ((virt | phys | bytes) & (FOUR_KB - 1)) return false;
    while (bytes) {
        bool big = !((virt | phys) & (TWO_MB - 1)) && bytes >= TWO_MB;
        u64 step = big ? TWO_MB : FOUR_KB;
        if (!(big ? map_2m_in(s, virt, phys, flags) : map_4k_in(s, virt, phys, flags))) return false;
        virt += step;
        phys += step;
        bytes -= step;
    }
    return true;
}

static bool translate_in(const Space& s, u64 virt_addr, Mapping* out) {
    u64* table = s.root;
    u64 writable = WRITABLE;
    u64 user = USER;
    u64 no_exec = 0;
    for (int shift = 39; shift >= 12; shift -= 9) {
        u64 e = table[(virt_addr >> shift) & 0x1FF];
        if (!(e & PRESENT)) return false;
        writable &= e;
        user &= e;
        no_exec |= e & NO_EXECUTE;
        bool leaf = shift == 12 || (shift != 39 && (e & HUGE_PAGE));
        if (!leaf) { table = virt_of(e & PHYS_MASK); continue; }
        u64 size = 1ull << shift;
        u64 pat_bit = shift == 12 ? PAT_4K : PAT_2M;
        out->phys = (e & PHYS_MASK & ~(size - 1)) | (virt_addr & (size - 1));
        out->size = size;
        out->flags = (e & (PRESENT | WRITE_THROUGH | NO_CACHE)) | writable | user | no_exec;
        out->pat = (e & pat_bit) != 0;
        return true;
    }
    return false;
}

bool map_2m(u64 virt_addr, u64 phys_addr, u64 flags) {
    Guard g;
    return map_2m_in(g_core, virt_addr, phys_addr, flags);
}

bool map_4k(u64 virt_addr, u64 phys_addr, u64 flags) {
    Guard g;
    return map_4k_in(g_core, virt_addr, phys_addr, flags);
}

bool map_range(u64 virt_addr, u64 phys_addr, u64 bytes, u64 flags) {
    Guard g;
    return map_range_in(g_core, virt_addr, phys_addr, bytes, flags);
}

bool unmap(u64 virt_addr, u64 bytes) {
    Guard g;
    return unmap_in(g_core, virt_addr, bytes);
}

bool protect(u64 virt_addr, u64 bytes, u64 flags) {
    Guard g;
    return protect_in(g_core, virt_addr, bytes, flags);
}

bool translate(u64 virt_addr, Mapping* out) {
    Guard g;
    return translate_in(g_core, virt_addr, out);
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
        if (!map_range_in(g_core, hhdm_offset + cur.lo, cur.lo, cur.hi - cur.lo, HHDM_FLAGS)) die("could not build the direct map");
        record_range(cur.lo, cur.hi);
        mapped += cur.hi - cur.lo;
        if (!last) cur = g_scratch[i];
    }
    return mapped;
}

static void map_image_section(const char* name, u64 start, u64 end, u64 flags, const char* perms) {
    if (end <= start) return;
    u64 phys = g_image.phys_base + (start - g_image.virt_base);
    if (!map_range_in(g_core, start, phys, end - start, flags)) die("could not map the kernel image");
    serial::printf("[constellation] image %s %p - %p %s\n", name, start, end, perms);
}

void init(u64 hhdm_offset, const universe::MemmapEntry* entries, u64 count, const KernelImage& image) {
    Guard g;
    g_hhdm = hhdm_offset;
    g_image = image;
    u64* root = new_table();
    if (!root) die("no memory for the top-level page table");
    g_core.root = root;
    g_core.root_phys = phys_of(root);
    g_core.in_use = true;

    for (u32 i = KERNEL_SLOT_FIRST; i < 512; ++i) {
        u64* t = new_table();
        if (!t) die("no memory for the shared kernel page directories");
        root[i] = phys_of(t) | PRESENT | WRITABLE;
    }

    u64 mapped = build_direct_map(hhdm_offset, entries, count);

    map_image_section(".requests", image.requests_start, image.requests_end, WRITABLE | NO_EXECUTE, "RW NX");
    map_image_section(".text", image.text_start, image.text_end, 0, "R  X");
    map_image_section(".rodata", image.rodata_start, image.rodata_end, NO_EXECUTE, "R  NX");
    map_image_section(".data/.bss", image.data_start, image.data_end, WRITABLE | NO_EXECUTE, "RW NX");

    for (u32 i = 0; i < g_range_count; ++i)
        serial::printf("[constellation] direct map %p - %p RW NX (%lu KiB)\n",
                        g_ranges[i].lo, g_ranges[i].hi, (g_ranges[i].hi - g_ranges[i].lo) / 1024);
    if (g_range_overflow) serial::printf("[constellation] %u further direct-map ranges not listed\n", g_range_overflow);

    cpu::write_cr3(g_core.root_phys);
    serial::printf("[constellation] The Core is live: %lu MiB direct-mapped in %u range(s), NX %s, CR3=%p\n",
                    mapped / (1024 * 1024), g_range_count + g_range_overflow,
                    percore::nx_enabled() ? "on" : "off", g_core.root_phys);
}

static void* map_firmware_in(u64 phys, u64 bytes, bool force_window) {
    if (!bytes || bytes > FIRMWARE_MAX_BYTES || phys + bytes < phys) return nullptr;
    u64 first = phys & ~(FOUR_KB - 1);
    u64 last = (phys + bytes + FOUR_KB - 1) & ~(FOUR_KB - 1);
    if (!force_window) {
        bool covered = true;
        Mapping m{};
        for (u64 p = first; p < last && covered; p += FOUR_KB)
            covered = translate_in(g_core, g_hhdm + p, &m) && m.phys == p && (m.flags & PRESENT);
        if (covered) return reinterpret_cast<void*>(g_hhdm + phys);
    }
    for (u32 i = 0; i < g_fw_count; ++i)
        if (phys >= g_fw[i].phys && phys + bytes <= g_fw[i].phys + g_fw[i].bytes)
            return reinterpret_cast<void*>(g_fw[i].virt + (phys - g_fw[i].phys));

    u32 pages = static_cast<u32>((last - first) / FOUR_KB);
    if (g_fw_pages + pages > FIRMWARE_WINDOW_PAGES) return nullptr;
    u64 base = FIRMWARE_REGION_BASE + static_cast<u64>(g_fw_pages) * FOUR_KB;
    for (u32 i = 0; i < pages; ++i)
        if (!map_4k_in(g_core, base + static_cast<u64>(i) * FOUR_KB, first + static_cast<u64>(i) * FOUR_KB, FIRMWARE_FLAGS)) return nullptr;
    g_fw_pages += pages;
    if (g_fw_count < FIRMWARE_CACHE) g_fw[g_fw_count++] = { first, last - first, base };
    return reinterpret_cast<void*>(base + (phys - first));
}

void* map_firmware(u64 phys_addr, u64 bytes) {
    Guard g;
    return map_firmware_in(phys_addr, bytes, false);
}

Space* kernel_space() { return &g_core; }

static bool user_range_ok(u64 virt, u64 bytes) {
    return bytes && !((virt | bytes) & (FOUR_KB - 1)) && virt < USER_LIMIT && bytes <= USER_LIMIT - virt;
}

static bool valid_user(const Space* s) { return s && s != &g_core && s >= g_spaces && s < g_spaces + MAX_SPACES && s->in_use; }

Space* create_space() {
    Guard g;
    for (auto& s : g_spaces) {
        if (s.in_use) continue;
        u64* root = new_table();
        if (!root) return nullptr;
        for (u32 i = KERNEL_SLOT_FIRST; i < 512; ++i) root[i] = g_core.root[i];
        s.root = root;
        s.root_phys = phys_of(root);
        s.cpumask = 0;
        s.in_use = true;
        return &s;
    }
    return nullptr;
}

static void free_user_tables(u64* root) {
    for (u32 i = 0; i < KERNEL_SLOT_FIRST; ++i) {
        if (!(root[i] & PRESENT)) continue;
        u64* pdpt = virt_of(root[i] & PHYS_MASK);
        for (u32 j = 0; j < 512; ++j) {
            if (!(pdpt[j] & PRESENT)) continue;
            u64* pd = virt_of(pdpt[j] & PHYS_MASK);
            for (u32 k = 0; k < 512; ++k)
                if ((pd[k] & PRESENT) && !(pd[k] & HUGE_PAGE)) universe::free(pd[k] & PHYS_MASK, 0);
            universe::free(pdpt[j] & PHYS_MASK, 0);
        }
        universe::free(root[i] & PHYS_MASK, 0);
    }
}

void destroy_space(Space* space) {
    Guard g;
    if (!valid_user(space)) die("destroy_space on something that is not a live address space");
    if (space->cpumask) die("destroy_space while the address space is loaded on a core");
    free_user_tables(space->root);
    universe::free(space->root_phys, 0);
    *space = Space{};
}

static bool user_flags_ok(u64 flags, u64 allowed) {
    if (flags & ~allowed) return false;
    return !((flags & WRITABLE) && !(flags & NO_EXECUTE));
}

bool space_map(Space* space, u64 virt_addr, u64 phys_addr, u64 bytes, u64 flags) {
    if (!user_range_ok(virt_addr, bytes) || (phys_addr & (FOUR_KB - 1))) return false;
    if (!user_flags_ok(flags, WRITABLE | NO_EXECUTE | CACHE_BITS)) return false;
    Guard g;
    if (!valid_user(space)) return false;
    if (!map_range_in(*space, virt_addr, phys_addr, bytes, flags)) {
        u64 done = 0;
        Mapping m{};
        while (done < bytes && translate_in(*space, virt_addr + done, &m) && m.phys == phys_addr + done) done += FOUR_KB;
        if (done) unmap_in(*space, virt_addr, done);
        return false;
    }
    return true;
}

bool space_unmap(Space* space, u64 virt_addr, u64 bytes) {
    if (!user_range_ok(virt_addr, bytes)) return false;
    Guard g;
    return valid_user(space) && unmap_in(*space, virt_addr, bytes);
}

bool space_protect(Space* space, u64 virt_addr, u64 bytes, u64 flags) {
    if (!user_range_ok(virt_addr, bytes)) return false;
    if (!user_flags_ok(flags, WRITABLE | NO_EXECUTE)) return false;
    Guard g;
    return valid_user(space) && protect_in(*space, virt_addr, bytes, flags);
}

bool space_translate(Space* space, u64 virt_addr, Mapping* out) {
    Guard g;
    const Space* s = space ? space : &g_core;
    if (s != &g_core && !valid_user(space)) return false;
    return translate_in(*s, virt_addr, out);
}

u64 space_root_phys(const Space* space) { return space ? space->root_phys : g_core.root_phys; }
u64 space_cpumask(const Space* space) { return space ? space->cpumask : 0; }

void activate(Space* space) {
    Space* target = space ? space : &g_core;
    u64 flags = cpu::irq_save();
    u32 me = tlb::core_index();
    bool tracked = me < tlb::MAX_CORES;
    Space* old = tracked && g_active[me] ? g_active[me] : &g_core;
    u64 bit = tracked ? 1ull << me : 0;
    if (target != &g_core) __atomic_or_fetch(&target->cpumask, bit, __ATOMIC_SEQ_CST);
    cpu::write_cr3(target->root_phys);
    if (old != target && old != &g_core) __atomic_and_fetch(&old->cpumask, ~bit, __ATOMIC_SEQ_CST);
    if (tracked) g_active[me] = target == &g_core ? nullptr : target;
    cpu::irq_restore(flags);
}

Space* active_space() {
    u32 me = tlb::core_index();
    if (me >= tlb::MAX_CORES || !g_active[me]) return &g_core;
    return g_active[me];
}

bool in_stack_guard(u64 addr) {
    if (addr < STACK_REGION_BASE || addr >= STACK_REGION_BASE + STACK_SLOT_BYTES * STACK_SLOTS) return false;
    u64 off = (addr - STACK_REGION_BASE) % STACK_SLOT_BYTES;
    return off < STACK_GUARD_BYTES || off >= STACK_GUARD_BYTES + STACK_BYTES;
}

static bool page_is(u64 virt, u64 phys, bool writable, bool executable) {
    Mapping m{};
    if (!translate_in(g_core, virt, &m) || m.phys != phys || !(m.flags & PRESENT)) return false;
    if (m.flags & USER) return false;
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
    ok = ok && !translate_in(g_core, g_hhdm + g_image.phys_base, &m);
    ok = ok && section_is(g_image.requests_start, g_image.requests_end, true, false);
    ok = ok && section_is(g_image.text_start, g_image.text_end, false, true);
    ok = ok && section_is(g_image.rodata_start, g_image.rodata_end, false, false);
    ok = ok && section_is(g_image.data_start, g_image.data_end, true, false);
    return ok;
}

static bool nx_matches(const Mapping& m, bool executable) {
    return !percore::nx_enabled() || ((m.flags & NO_EXECUTE) == 0) == executable;
}

static bool selftest_mappings() {
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

    ok = ok && protect(WINDOW + 0x2000, 0x2000, NO_EXECUTE);
    ok = ok && translate(WINDOW + 0x2000, &m) && !(m.flags & WRITABLE) && (m.flags & NO_CACHE) && m.pat;
    ok = ok && translate(WINDOW + 0x3FFF, &m) && !(m.flags & WRITABLE);
    ok = ok && translate(WINDOW + 0x1000, &m) && (m.flags & WRITABLE);
    ok = ok && translate(WINDOW + 0x4000, &m) && (m.flags & WRITABLE);
    ok = ok && !protect(WINDOW + 0x2000, 0x1000, USER);
    ok = ok && !protect(UNUSED_VA, 0x1000, NO_EXECUTE);
    ok = ok && !protect(WINDOW + 0x1F8000, 0x10000, NO_EXECUTE);

    ok = ok && unmap(WINDOW + 0x1000, 0x2000);
    ok = ok && !translate(WINDOW + 0x1000, &m) && !translate(WINDOW + 0x2FFF, &m);
    ok = ok && translate(WINDOW, &m) && translate(WINDOW + 0x3000, &m);
    ok = ok && !unmap(WINDOW + 0x1000, 0x1000);
    ok = ok && !unmap(WINDOW + 0x2800, 0x1000);
    ok = ok && unmap(WINDOW + 0x5000, 0x1000) && !translate(WINDOW + 0x5000, &m);

    ok = ok && map_4k(WINDOW, PHYS_B, WRITABLE | NO_EXECUTE);
    ok = ok && translate(WINDOW + 8, &m) && m.phys == PHYS_B + 8 && !m.pat && !(m.flags & NO_CACHE);
    return ok;
}

static bool selftest_firmware() {
    bool ok = true;
    u64 frame = universe::alloc_zeroed(0);
    if (!frame) return false;
    u64* alias = virt_of(frame);
    constexpr u64 MAGIC = 0xA9E110FA9E110FA1ull;

    void* direct = map_firmware(frame + 0x10, 8);
    ok = ok && direct == reinterpret_cast<void*>(g_hhdm + frame + 0x10);

    void* win = nullptr;
    {
        Guard g;
        win = map_firmware_in(frame + 0x10, 8, true);
    }
    ok = ok && win && reinterpret_cast<u64>(win) >= FIRMWARE_REGION_BASE;
    if (win) {
        Mapping m{};
        ok = ok && translate(reinterpret_cast<u64>(win), &m) && m.phys == frame + 0x10 && (m.flags & NO_CACHE) && !(m.flags & WRITABLE);
        cpu::clflush(alias);
        alias[2] = MAGIC;
        cpu::clflush(&alias[2]);
        ok = ok && cpu::read_u64(reinterpret_cast<u64>(win)) == MAGIC;
        Guard g;
        ok = ok && map_firmware_in(frame + 0x18, 8, true) == static_cast<char*>(win) + 8;
    }
    ok = ok && map_firmware(frame, 0) == nullptr;
    ok = ok && map_firmware(~0ull - 4, 64) == nullptr;
    ok = ok && map_firmware(frame, FIRMWARE_MAX_BYTES + 1) == nullptr;
    universe::free(frame, 0);
    return ok;
}

static bool selftest_spaces() {
    constexpr u64 UVA = 0x0000000010000000ull;
    constexpr u64 UVA_BIG = 0x0000000040000000ull;
    u64 before = universe::free_bytes();
    bool ok = true;
    Mapping m{};
    u64 frames[4];
    for (auto& f : frames) { f = universe::alloc_zeroed(0); ok = ok && f; }
    if (!ok) return false;

    Space* a = create_space();
    Space* b = create_space();
    ok = ok && a && b && a != b && space_root_phys(a) != space_root_phys(b) && space_root_phys(a) != g_core.root_phys;
    if (!a || !b) return false;

    for (u32 i = KERNEL_SLOT_FIRST; i < 512; ++i)
        ok = ok && a->root[i] == g_core.root[i] && b->root[i] == g_core.root[i];
    for (u32 i = 0; i < KERNEL_SLOT_FIRST; ++i) ok = ok && !(a->root[i] & PRESENT);

    ok = ok && space_map(a, UVA, frames[0], 0x1000, WRITABLE | NO_EXECUTE);
    ok = ok && space_map(a, UVA + 0x1000, frames[1], 0x1000, NO_EXECUTE);
    ok = ok && space_map(a, UVA + 0x2000, frames[2], 0x1000, 0);
    ok = ok && space_translate(a, UVA, &m) && m.phys == frames[0] && (m.flags & USER) && (m.flags & WRITABLE) && nx_matches(m, false);
    ok = ok && space_translate(a, UVA + 0x1000, &m) && (m.flags & USER) && !(m.flags & WRITABLE);
    ok = ok && space_translate(a, UVA + 0x2008, &m) && m.phys == frames[2] + 8 && (m.flags & USER) && !(m.flags & WRITABLE) && nx_matches(m, true);
    ok = ok && !space_translate(b, UVA, &m);
    ok = ok && !translate(UVA, &m);

    ok = ok && !space_map(a, UVA + 0x3000, frames[3], 0x1000, WRITABLE);
    ok = ok && !space_map(a, UVA + 0x3000, frames[3], 0x1000, WRITABLE | NO_EXECUTE | USER);
    ok = ok && !space_map(a, USER_LIMIT, frames[3], 0x1000, NO_EXECUTE);
    ok = ok && !space_map(a, USER_LIMIT - 0x1000, frames[3], 0x2000, NO_EXECUTE);
    ok = ok && !space_map(a, 0xFFFF800000000000ull, frames[3], 0x1000, NO_EXECUTE);
    ok = ok && !space_map(a, UVA + 0x3001, frames[3], 0x1000, NO_EXECUTE);
    ok = ok && !space_map(a, UVA + 0x3000, frames[3] + 1, 0x1000, NO_EXECUTE);
    ok = ok && !space_map(nullptr, UVA, frames[3], 0x1000, NO_EXECUTE);
    ok = ok && !space_map(&g_core, UVA, frames[3], 0x1000, NO_EXECUTE);

    ok = ok && space_protect(a, UVA + 0x1000, 0x1000, WRITABLE | NO_EXECUTE);
    ok = ok && space_translate(a, UVA + 0x1000, &m) && (m.flags & WRITABLE) && (m.flags & USER);
    ok = ok && !space_protect(a, UVA + 0x1000, 0x1000, WRITABLE);
    ok = ok && !space_protect(a, UVA + 0x10000, 0x1000, NO_EXECUTE);
    ok = ok && space_unmap(a, UVA + 0x1000, 0x1000) && !space_translate(a, UVA + 0x1000, &m);
    ok = ok && !space_unmap(a, UVA + 0x1000, 0x1000);

    ok = ok && space_map(a, UVA_BIG, 0x40000000ull, 0x400000, WRITABLE | NO_EXECUTE);
    ok = ok && space_translate(a, UVA_BIG + 0x1234, &m) && m.size == 0x200000 && (m.flags & USER) && m.phys == 0x40000000ull + 0x1234;
    ok = ok && space_unmap(a, UVA_BIG + 0x1000, 0x1000);
    ok = ok && !space_translate(a, UVA_BIG + 0x1000, &m) && space_translate(a, UVA_BIG, &m) && m.size == 0x1000 && (m.flags & USER);
    ok = ok && space_translate(a, UVA_BIG + 0x2000, &m) && m.size == 0x1000 && m.phys == 0x40000000ull + 0x2000;
    ok = ok && space_unmap(a, UVA_BIG + 0x200000, 0x200000) && !space_translate(a, UVA_BIG + 0x300000, &m);

    Mapping k{};
    if (g_hhdm >= USER_LIMIT) ok = ok && translate(g_hhdm, &k) && space_translate(a, g_hhdm, &m) && m.phys == k.phys && !(m.flags & USER);
    ok = ok && space_translate(a, g_image.text_start, &m) && !(m.flags & USER);

    destroy_space(b);
    ok = ok && !space_map(b, UVA, frames[3], 0x1000, NO_EXECUTE);
    Space* c = create_space();
    ok = ok && c == b && !space_translate(c, UVA, &m);
    destroy_space(c);
    destroy_space(a);
    for (auto f : frames) universe::free(f, 0);
    ok = ok && universe::free_bytes() == before;
    return ok;
}

static bool stage(const char* name, bool ok) {
    if (!ok) serial::printf("[constellation] selftest stage failed: %s\n", name);
    return ok;
}

bool selftest() {
    bool ok = stage("mappings", selftest_mappings());
    ok = stage("firmware window", selftest_firmware()) && ok;
    ok = stage("address spaces", selftest_spaces()) && ok;
    ok = stage("layout", verify_layout()) && ok;
    return ok;
}

bool smp_selftest() {
    constexpr u64 VA = 0xFFFFE00000000000ull + 0x10000000ull;
    constexpr u64 A = 0x1111AAAA1111AAAAull;
    constexpr u64 B = 0x2222BBBB2222BBBBull;
    u64 f1 = universe::alloc_zeroed(0);
    u64 f2 = universe::alloc_zeroed(0);
    if (!f1 || !f2) return false;
    *virt_of(f1) = A;
    *virt_of(f2) = B;
    u64 seen[tlb::MAX_CORES];
    bool ok = map_4k(VA, f1, WRITABLE | NO_EXECUTE);

    auto all_read = [&](u64 expect) {
        for (auto& v : seen) v = 0;
        if (!tlb::selftest_probe(VA, seen, tlb::MAX_CORES)) return false;
        u64 online = tlb::online_mask();
        for (u32 i = 0; i < tlb::MAX_CORES; ++i)
            if ((online & (1ull << i)) && seen[i] != expect) return false;
        return true;
    };

    ok = ok && all_read(A);
    ok = ok && map_4k(VA, f2, WRITABLE | NO_EXECUTE);
    ok = ok && all_read(B);
    ok = ok && map_4k(VA, f1, WRITABLE | NO_EXECUTE);
    ok = ok && all_read(A);
    ok = ok && protect(VA, 0x1000, NO_EXECUTE);
    ok = ok && all_read(A);
    Mapping m{};
    ok = ok && translate(VA, &m) && !(m.flags & WRITABLE);
    ok = ok && unmap(VA, 0x1000) && !translate(VA, &m);

    Space* sp = create_space();
    ok = ok && sp;
    if (sp) {
        u32 me = tlb::core_index();
        u64 root = space_root_phys(sp);
        activate(sp);
        ok = ok && cpu::read_cr3() == root && active_space() == sp;
        ok = ok && me < tlb::MAX_CORES && (space_cpumask(sp) & (1ull << me));
        ok = ok && *virt_of(f1) == A;
        activate(nullptr);
        ok = ok && cpu::read_cr3() == g_core.root_phys && active_space() == &g_core;
        ok = ok && space_cpumask(sp) == 0;
        destroy_space(sp);
    }
    universe::free(f1, 0);
    universe::free(f2, 0);
    return ok;
}

u64 core_pml4_phys() { return g_core.root_phys; }

}
