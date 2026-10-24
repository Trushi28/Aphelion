#include <limine.h>
#include <cosmos/types.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/framebuffer.hpp>
#include <cosmos/gdt.hpp>
#include <cosmos/percore.hpp>
#include <cosmos/fpu.hpp>
#include <cosmos/idt.hpp>
#include <cosmos/tlb.hpp>
#include <cosmos/apic.hpp>
#include <cosmos/acpi.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/kheap.hpp>
#include <cosmos/bootcfg.hpp>
#include <cosmos/timesrc.hpp>
#include <cosmos/vmm.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/orbital.hpp>
#include <cosmos/ioapic.hpp>
#include <cosmos/keyboard.hpp>
#include <cosmos/input.hpp>
#include <cosmos/power.hpp>
#include <cosmos/helm.hpp>
#include <cosmos/nebula_realms.hpp>
#include <cosmos/lens_text.hpp>
#include <cosmos/console_stream.hpp>
#include <cosmos/corona.hpp>
#include <cosmos/pci.hpp>
#include <cosmos/virtio_blk.hpp>
#include <cosmos/ahci.hpp>
#include <cosmos/nvme.hpp>
#include <cosmos/blockdev.hpp>
#include <cosmos/stellar.hpp>
#include <cosmos/xhci.hpp>
#include <cosmos/usbstor.hpp>
#include <cosmos/clock.hpp>
#include <cosmos/civil.hpp>

extern "C" char __kernel_start[];
extern "C" char __kernel_end[];
extern "C" char __requests_start[];
extern "C" char __requests_end[];
extern "C" char __text_start[];
extern "C" char __text_end[];
extern "C" char __rodata_start[];
extern "C" char __rodata_end[];
extern "C" char __data_start[];
extern "C" char __data_end[];

__attribute__((used, section(".requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".requests")))
static volatile limine_framebuffer_request fb_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST, .revision = 0, .response = nullptr
};
__attribute__((used, section(".requests")))
static volatile limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST, .revision = 0, .response = nullptr
};
__attribute__((used, section(".requests")))
static volatile limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST, .revision = 0, .response = nullptr
};
__attribute__((used, section(".requests")))
static volatile limine_executable_cmdline_request cmdline_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST, .revision = 0, .response = nullptr
};
__attribute__((used, section(".requests")))
static volatile limine_kernel_file_request exec_file_request = {
    .id = LIMINE_KERNEL_FILE_REQUEST, .revision = 0, .response = nullptr
};
__attribute__((used, section(".requests")))
static volatile limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST, .revision = 0, .response = nullptr
};
__attribute__((used, section(".requests")))
static volatile limine_kernel_address_request kaddr_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST, .revision = 0, .response = nullptr
};
__attribute__((used, section(".requests")))
static volatile limine_smp_request smp_request = {
    .id = LIMINE_SMP_REQUEST, .revision = 0, .response = nullptr, .flags = LIMINE_SMP_X2APIC
};

__attribute__((used, section(".requests_start_marker")))
static volatile LIMINE_REQUESTS_START_MARKER;
__attribute__((used, section(".requests_end_marker")))
static volatile LIMINE_REQUESTS_END_MARKER;

static volatile u64 g_cpus_online = 0;
static volatile u64 g_next_core_idx = 1;
static u64 g_hhdm_offset = 0;

static constexpr u64 MAX_MEMMAP_ENTRIES = 512;
static universe::MemmapEntry g_entries[MAX_MEMMAP_ENTRIES];
static u64 g_n_entries = 0;
static u64 g_expected_cores = 1;

static constexpr u64 TEST_TAIL_SECTORS = 64;
static constexpr const char* HELLO_REWRITTEN = "Aphelion Stellar FS -- rewritten after snapshot.\n";

static void map_device_page_or_die(u64 phys, const char* what) {
    u64 base = phys & ~0xFFFull;
    if (!constellation::map_4k(g_hhdm_offset + base, base,
                               constellation::WRITABLE | constellation::NO_CACHE | constellation::NO_EXECUTE)) {
        serial::printf("[boot] FATAL: could not map the %s page at %p\n", what, base);
        cpu::hang();
    }
}

static u32 init_gdt_idt(bool is_bsp) {
    u32 core_idx = is_bsp ? 0 : static_cast<u32>(__atomic_fetch_add(&g_next_core_idx, 1, __ATOMIC_SEQ_CST));
    gdt::init(core_idx);
    idt::init();
    return core_idx;
}

static void bring_up_core(bool is_bsp) {
    init_gdt_idt(is_bsp);
    apic::init(g_hhdm_offset);
}

extern "C" const char aphelion_build_id[];

static bool boot_disk_guid(u8* out) {
    if (!exec_file_request.response || !exec_file_request.response->kernel_file) return false;
    const limine_uuid& id = exec_file_request.response->kernel_file->gpt_disk_uuid;
    out[0] = static_cast<u8>(id.a); out[1] = static_cast<u8>(id.a >> 8); out[2] = static_cast<u8>(id.a >> 16); out[3] = static_cast<u8>(id.a >> 24);
    out[4] = static_cast<u8>(id.b); out[5] = static_cast<u8>(id.b >> 8);
    out[6] = static_cast<u8>(id.c); out[7] = static_cast<u8>(id.c >> 8);
    bool any = false;
    for (int i = 0; i < 8; ++i) { out[8 + i] = id.d[i]; }
    for (int i = 0; i < 16; ++i) if (out[i]) any = true;
    return any;
}

static void start_usb_storage() {
    if (timesrc::tsc_khz() == 0) timesrc::calibrate(10, 4, false);
    if (timesrc::tsc_khz() == 0) {
        serial::writeln("[storage] storage=usb requested but no time source could be measured, so USB timeouts cannot be trusted");
        fb::printf(0xE0D080, "[--] USB storage: no usable time source for USB timeouts\n");
        return;
    }
    if (!xhci::init(g_hhdm_offset)) {
        serial::writeln("[storage] storage=usb requested but no xHCI controller started");
        fb::printf(0xE0D080, "[--] USB storage: no xHCI controller could be started\n");
        return;
    }
    u8 guid[16];
    bool have_guid = boot_disk_guid(guid);
    char volume_id[24];
    u32 n = 0;
    volume_id[n++] = 'A'; volume_id[n++] = 'P'; volume_id[n++] = 'H';
    for (u32 i = 0; aphelion_build_id[i] && n + 1 < sizeof(volume_id); ++i) volume_id[n++] = aphelion_build_id[i];
    volume_id[n] = 0;
    serial::printf("[storage] identifying the boot stick by %s%s\n", have_guid ? "disk GUID" : "volume id only (Limine reported no GPT disk GUID)",
                    have_guid ? ", falling back to volume id" : "");
    usbstor::init(have_guid ? guid : nullptr, volume_id);
    if (usbstor::window_present()) {
        const usbstor::LunInfo* w = usbstor::window_lun();
        fb::printf(0xC0FFC0, "[ok] USB storage window: %lu MiB free after the boot image (%s %s)\n", w->tail_sectors / 2048, w->vendor, w->product);
    } else {
        fb::printf(0xE0D080, "[--] USB storage: %s\n", usbstor::window_status());
    }
}

extern "C" void ap_entry(limine_smp_info* info) {
    percore::early_init();
    fpu::init_core();
    cpu::write_cr3(constellation::core_pml4_phys());
    bring_up_core(false);
    tlb::init_core();
    orbital::init_core();

    u64 n = __atomic_add_fetch(&g_cpus_online, 1, __ATOMIC_SEQ_CST);
    serial::printf("[smp] Sun %lu online (lapic id %u), %s mode\n", n, info->lapic_id,
                    apic::using_x2apic() ? "x2APIC" : "xAPIC");
    orbital::start_core();
}

static u32 itoa_dec(u64 v, char* out) {
    char tmp[20];
    u32 len = 0;
    if (v == 0) tmp[len++] = '0';
    while (v) { tmp[len++] = static_cast<char>('0' + (v % 10)); v /= 10; }
    for (u32 i = 0; i < len; ++i) out[i] = tmp[len - 1 - i];
    return len;
}

static void count_entry(const char*, u64, u32, void* ctx) {
    ++(*static_cast<u64*>(ctx));
}

static u64 cstr_len(const char* s) {
    u64 n = 0;
    while (s[n]) ++n;
    return n;
}

static char g_disk_note[112];

static void set_disk_note(const char* a, const char* b) {
    u32 n = 0;
    for (const char* p = a; p && *p && n + 1 < sizeof(g_disk_note); ++p) g_disk_note[n++] = *p;
    for (const char* p = b; p && *p && n + 1 < sizeof(g_disk_note); ++p) g_disk_note[n++] = *p;
    g_disk_note[n] = 0;
}

static bool file_equals(u64 star, const char* expect, u64 snap = stellar::LIVE) {
    u8 tmp[256];
    u64 want = cstr_len(expect);
    if (star == stellar::INVALID_STAR || want >= sizeof(tmp)) return false;
    u64 got = stellar::read_file(star, tmp, sizeof(tmp), snap);
    if (got != want) return false;
    for (u64 i = 0; i < got; ++i)
        if (tmp[i] != static_cast<u8>(expect[i])) return false;
    return true;
}

static NORETURN void panic_no_framebuffer() {
    serial::writeln("[boot] FATAL: bootloader gave no framebuffer");
    cpu::hang();
}

static void demo_cooperative(void* arg) {
    const char* label = static_cast<const char*>(arg);
    for (u32 i = 0; i < 8; ++i) {
        serial::printf("[demo] %s (cooperative) round %u on core %d\n",
                        label, i, static_cast<int>(apic::id()));
        orbital::yield();
    }
    serial::printf("[demo] %s done (returning -- implicit exit)\n", label);
}

static void demo_cooperative_explicit_exit(void* arg) {
    const char* label = static_cast<const char*>(arg);
    for (u32 i = 0; i < 8; ++i) {
        serial::printf("[demo] %s (cooperative) round %u on core %d\n",
                        label, i, static_cast<int>(apic::id()));
        orbital::yield();
    }
    serial::printf("[demo] %s done (calling exit_current explicitly)\n", label);
    orbital::exit_current();
}

static void demo_spinner(void* arg) {
    const char* label = static_cast<const char*>(arg);
    u64 spins = 0;
    u32 prints = 0;
    for (;;) {
        ++spins;
        if ((spins & 0xFFFFF) == 0 && prints < 8) {
            serial::printf("[demo] %s (never yields -- only timer-preempted) core %d, spins=%lu\n",
                            label, static_cast<int>(apic::id()), spins);
            ++prints;
        }
    }
}

constexpr u64 CLOCK_TEST_MS = 50;
constexpr u64 CLOCK_TEST_SPINS = 300000000ull;

static void demo_clock(void*) {
    u64 start = clock::uptime_ms();
    u64 prev = start;
    bool monotonic = true;
    u64 spins = 0;
    while (clock::uptime_ms() - start < CLOCK_TEST_MS && spins < CLOCK_TEST_SPINS) {
        u64 now = clock::uptime_ms();
        if (now < prev) monotonic = false;
        prev = now;
        ++spins;
    }
    u64 advanced = clock::uptime_ms() - start;
    bool ok = monotonic && advanced >= CLOCK_TEST_MS;
    serial::printf("[clock] BSP tick counter advanced %lu ms in the test window, %s\n",
                    advanced, monotonic ? "monotonic" : "went backwards");
    serial::printf("[selftest] clock ticks: %s\n", ok ? "ok" : "MISMATCH");
    orbital::exit_current();
}

constexpr u64 WORKER_COUNT = 8;
constexpr u64 WORKER_ITERATIONS_SINGLE = 50000;
constexpr u64 WORKER_ITERATIONS_MIN = 2000000;
constexpr u64 WORKER_ITERATIONS_CAP = 600000000;
static volatile u64 g_worker_cores_mask = 0;
static volatile u32 g_workers_remaining = WORKER_COUNT;

static u32 popcount64(u64 m) {
    u32 n = 0;
    while (m) { n += static_cast<u32>(m & 1); m >>= 1; }
    return n;
}

static void note_core(u32 core) {
    __atomic_fetch_or(&g_worker_cores_mask, 1ull << (core < 64 ? core : 63), __ATOMIC_SEQ_CST);
}

static void demo_worker(void* arg) {
    u64 idx = reinterpret_cast<u64>(arg);
    serial::printf("[demo] worker %lu started on core %d\n", idx, static_cast<int>(apic::id()));
    const bool expect_migration = g_cpus_online > 1;
    const u64 cap = expect_migration ? WORKER_ITERATIONS_CAP : WORKER_ITERATIONS_SINGLE;
    for (u64 i = 0; i < cap; ++i) {
        if ((i & 0x7FF) == 0) {
            note_core(apic::id());
            if (expect_migration && i >= WORKER_ITERATIONS_MIN && popcount64(g_worker_cores_mask) > 1) break;
            orbital::yield();
        }
    }
    u32 core = apic::id();
    note_core(core);
    serial::printf("[demo] worker %lu finished on core %d\n", idx, static_cast<int>(core));
    if (__atomic_sub_fetch(&g_workers_remaining, 1u, __ATOMIC_SEQ_CST) == 0) {
        u32 distinct = popcount64(g_worker_cores_mask);
        bool ok = distinct > 1;
        serial::printf("[orbital] work-stealing check: %lu worker(s) ran across %u distinct core(s) -> %s\n",
                        WORKER_COUNT, distinct, ok ? "stealing confirmed" : "no migration observed");
    }
    orbital::exit_current();
}

constexpr u64 FS_STRESS_WORKERS = 4;
constexpr u64 FS_STRESS_FILES = 25;
static bool g_fs_stress_enabled = false;
static u64 g_fs_stress_dir = stellar::INVALID_STAR;
static u64 g_fs_stress_alt = stellar::INVALID_STAR;
static volatile u32 g_fs_stress_remaining = FS_STRESS_WORKERS;
static volatile u32 g_fs_stress_errors = 0;

static void fs_stress_name(char* out, u64 w, u64 j) {
    u32 n = 0;
    out[n++] = 's';
    n += itoa_dec(w, out + n);
    out[n++] = '_';
    n += itoa_dec(j, out + n);
    out[n] = 0;
}

static void fs_stress_fail(const char* why, u64 w, u64 j) {
    __atomic_fetch_add(&g_fs_stress_errors, 1u, __ATOMIC_SEQ_CST);
    serial::printf("[smp-fs] FAIL: %s (worker %lu, file %lu)\n", why, w, j);
}

static void fs_stress_finish() {
    const u64 NONE = stellar::INVALID_STAR;
    u64 total = FS_STRESS_WORKERS * FS_STRESS_FILES;
    u64 shared_in_dir = stellar::find(g_fs_stress_dir, "shared") != NONE ? 1 : 0;
    u64 shared_in_alt = stellar::find(g_fs_stress_alt, "shared") != NONE ? 1 : 0;
    if (shared_in_dir + shared_in_alt != 1) {
        serial::printf("[smp-fs] FAIL: the contended file exists in %lu directories, expected exactly one\n", shared_in_dir + shared_in_alt);
        __atomic_fetch_add(&g_fs_stress_errors, 1u, __ATOMIC_SEQ_CST);
    }
    u64 listed = 0, listed_alt = 0;
    stellar::list(g_fs_stress_dir, &count_entry, &listed);
    stellar::list(g_fs_stress_alt, &count_entry, &listed_alt);
    if (listed != total + 1 + shared_in_dir || listed_alt != shared_in_alt) {
        serial::printf("[smp-fs] FAIL: directories hold %lu and %lu entries, expected %lu and %lu\n",
                        listed, listed_alt, total + 1 + shared_in_dir, shared_in_alt);
        __atomic_fetch_add(&g_fs_stress_errors, 1u, __ATOMIC_SEQ_CST);
    }
    for (u64 w = 0; w < FS_STRESS_WORKERS; ++w) {
        for (u64 j = 0; j < FS_STRESS_FILES; ++j) {
            char name[24];
            fs_stress_name(name, w, j);
            u64 star = stellar::find(g_fs_stress_dir, name);
            if (!file_equals(star, name) || !stellar::verify_file(star)) fs_stress_fail("file missing or corrupt", w, j);
            if (stellar::find(g_fs_stress_alt, name) != NONE) fs_stress_fail("file also present in the second directory", w, j);
        }
    }
    u64 shared_home = shared_in_dir ? g_fs_stress_dir : g_fs_stress_alt;
    u64 shared_star = stellar::find(shared_home, "shared");
    if (!file_equals(shared_star, "shared") || !stellar::verify_file(shared_star)) fs_stress_fail("contended file damaged", 0, 0);
    if (!stellar::unlink(shared_home, "shared")) fs_stress_fail("could not remove the contended file", 0, 0);
    for (u64 w = 0; w < FS_STRESS_WORKERS; ++w) {
        for (u64 j = 0; j < FS_STRESS_FILES; ++j) {
            char name[24];
            fs_stress_name(name, w, j);
            if (!stellar::unlink(g_fs_stress_dir, name)) fs_stress_fail("unlink failed", w, j);
        }
    }
    if (!stellar::unlink(g_fs_stress_dir, "alt")) fs_stress_fail("could not remove the second directory", 0, 0);
    if (!stellar::unlink(stellar::ROOT_STAR, "smpstress")) fs_stress_fail("could not remove the directory", 0, 0);
    stellar::CheckReport report{};
    if (!stellar::check(&report, false) || !report.ok()) {
        serial::writeln("[smp-fs] FAIL: check() found damage after the concurrent renames");
        __atomic_fetch_add(&g_fs_stress_errors, 1u, __ATOMIC_SEQ_CST);
    }
    u32 errors = g_fs_stress_errors;
    serial::printf("[smp-fs] %lu workers created %lu files in one directory, moved each to a second directory and back, contended on one file, then verified and removed them: %u error(s)\n",
                    FS_STRESS_WORKERS, total, errors);
    serial::printf("[selftest] smp-fs: %s\n", errors == 0 ? "ok" : "MISMATCH");
}

static void demo_fs_stress(void* arg) {
    u64 w = reinterpret_cast<u64>(arg);
    for (u64 j = 0; j < FS_STRESS_FILES; ++j) {
        char name[24];
        fs_stress_name(name, w, j);
        u64 star = stellar::find(g_fs_stress_dir, name);
        if (star == stellar::INVALID_STAR)
            star = stellar::create_file(g_fs_stress_dir, name, name, cstr_len(name));
        if (star == stellar::INVALID_STAR) fs_stress_fail("create failed", w, j);
        else if (!file_equals(star, name)) fs_stress_fail("readback mismatch", w, j);
        if ((j % 5) == 0) orbital::yield();
    }
    for (u64 j = 0; j < FS_STRESS_FILES; ++j) {
        using stellar::Status;
        char name[24];
        fs_stress_name(name, w, j);
        u64 star = stellar::find(g_fs_stress_dir, name);
        if (star == stellar::INVALID_STAR) { fs_stress_fail("file missing before the rename phase", w, j); continue; }
        Status why = Status::Ok;
        if (!stellar::rename(g_fs_stress_dir, name, g_fs_stress_alt, name, stellar::RENAME_NOREPLACE, &why)) {
            fs_stress_fail("move to the second directory failed", w, j);
            continue;
        }
        if (stellar::find(g_fs_stress_alt, name) != star || stellar::find(g_fs_stress_dir, name) != stellar::INVALID_STAR)
            fs_stress_fail("moved file is not where the rename left it", w, j);
        if (!stellar::rename(g_fs_stress_alt, name, g_fs_stress_dir, name, stellar::RENAME_NOREPLACE, &why))
            fs_stress_fail("move back failed", w, j);
        else if (stellar::find(g_fs_stress_dir, name) != star)
            fs_stress_fail("file did not come back", w, j);
        if ((j % 5) == 0) {
            u64 from = ((w + j) & 1) ? g_fs_stress_dir : g_fs_stress_alt;
            u64 to = from == g_fs_stress_dir ? g_fs_stress_alt : g_fs_stress_dir;
            if (!stellar::rename(from, "shared", to, "shared", stellar::RENAME_NOREPLACE, &why) && why != Status::NotFound)
                fs_stress_fail("contended rename returned an unexpected status", w, j);
        }
        if ((j % 5) == 2) orbital::yield();
    }
    if (__atomic_sub_fetch(&g_fs_stress_remaining, 1u, __ATOMIC_SEQ_CST) == 0) fs_stress_finish();
    orbital::exit_current();
}

constexpr u64 MM_WORKERS = 4;
constexpr u32 MM_OPERATIONS = 20000;
constexpr u32 MM_LIVE = 24;
constexpr u32 MM_PAGE_BLOCKS = 8;
static volatile u32 g_mm_remaining = MM_WORKERS;
static volatile u32 g_mm_errors = 0;
static volatile u64 g_mm_operations = 0;
static u64 g_mm_baseline_objects = 0;

struct MmLive { u8* p; u32 size; u8 tag; };
struct MmPages { u64 phys; int order; u8 tag; };

static u64 mm_next(u64& s) {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return s;
}

static void mm_fail(const char* why, u64 w, u32 op) {
    __atomic_fetch_add(&g_mm_errors, 1u, __ATOMIC_SEQ_CST);
    serial::printf("[smp-mm] FAIL: %s (worker %lu, operation %u)\n", why, w, op);
}

static u32 mm_size(u64& rng) {
    u64 r = mm_next(rng) % 1000;
    if (r < 600) return 1 + static_cast<u32>(mm_next(rng) % 128);
    if (r < 900) return 129 + static_cast<u32>(mm_next(rng) % 1920);
    if (r < 980) return 2049 + static_cast<u32>(mm_next(rng) % 18000);
    return 20001 + static_cast<u32>(mm_next(rng) % 110000);
}

static void mm_fill(const MmLive& l) {
    u32 n = l.size < 192 ? l.size : 192;
    for (u32 i = 0; i < n; ++i) l.p[i] = static_cast<u8>(l.tag + i);
    if (l.size > 192) l.p[l.size - 1] = l.tag;
}

static bool mm_intact(const MmLive& l) {
    u32 n = l.size < 192 ? l.size : 192;
    for (u32 i = 0; i < n; ++i) if (l.p[i] != static_cast<u8>(l.tag + i)) return false;
    return l.size <= 192 || l.p[l.size - 1] == l.tag;
}

static void mm_stamp(const MmPages& b) {
    u64 pages = 1ull << b.order;
    for (u64 i = 0; i < pages; ++i) {
        auto* page = static_cast<volatile u8*>(universe::to_virt(b.phys + i * universe::PAGE_SIZE));
        for (u32 k = 0; k < 64; ++k) page[k] = static_cast<u8>(b.tag + k + i);
    }
}

static bool mm_stamp_intact(const MmPages& b) {
    u64 pages = 1ull << b.order;
    for (u64 i = 0; i < pages; ++i) {
        auto* page = static_cast<volatile u8*>(universe::to_virt(b.phys + i * universe::PAGE_SIZE));
        for (u32 k = 0; k < 64; ++k) if (page[k] != static_cast<u8>(b.tag + k + i)) return false;
    }
    return true;
}

static void demo_mm_stress(void* arg) {
    u64 w = reinterpret_cast<u64>(arg);
    u64 rng = 0x9E3779B97F4A7C15ull * (w + 1);
    MmLive live[MM_LIVE];
    MmPages blocks[MM_PAGE_BLOCKS];
    u32 n = 0, pn = 0;
    for (u32 op = 0; op < MM_OPERATIONS; ++op) {
        u64 r = mm_next(rng) % 100;
        if (r < 12) {
            if (pn < MM_PAGE_BLOCKS) {
                int order = static_cast<int>(mm_next(rng) % 4);
                u64 phys = universe::alloc(order);
                if (phys) {
                    MmPages b{ phys, order, static_cast<u8>(w * 67 + op) };
                    mm_stamp(b);
                    blocks[pn++] = b;
                }
            }
        } else if (r < 24) {
            if (pn) {
                u32 j = static_cast<u32>(mm_next(rng) % pn);
                if (!mm_stamp_intact(blocks[j])) mm_fail("a physical block was overwritten by another owner", w, op);
                universe::free(blocks[j].phys, blocks[j].order);
                blocks[j] = blocks[--pn];
            }
        } else if (n == 0 || (n < MM_LIVE && r < 62)) {
            MmLive l;
            l.size = mm_size(rng);
            l.p = static_cast<u8*>(kheap::alloc(l.size));
            if (l.p) {
                l.tag = static_cast<u8>(w * 91 + op);
                if (reinterpret_cast<u64>(l.p) % kheap::GUARANTEED_ALIGNMENT) mm_fail("misaligned heap pointer", w, op);
                mm_fill(l);
                live[n++] = l;
            }
        } else {
            u32 j = static_cast<u32>(mm_next(rng) % n);
            if (!mm_intact(live[j])) mm_fail("a heap object was overwritten by another owner", w, op);
            kheap::free(live[j].p);
            live[j] = live[--n];
        }
        if ((op & 63) == 0) orbital::yield();
    }
    for (u32 i = 0; i < n; ++i) {
        if (!mm_intact(live[i])) mm_fail("a heap object was overwritten before release", w, MM_OPERATIONS);
        kheap::free(live[i].p);
    }
    for (u32 i = 0; i < pn; ++i) {
        if (!mm_stamp_intact(blocks[i])) mm_fail("a physical block was overwritten before release", w, MM_OPERATIONS);
        universe::free(blocks[i].phys, blocks[i].order);
    }
    __atomic_fetch_add(&g_mm_operations, static_cast<u64>(MM_OPERATIONS), __ATOMIC_SEQ_CST);
    if (__atomic_sub_fetch(&g_mm_remaining, 1u, __ATOMIC_SEQ_CST) == 0) {
        bool balanced = kheap::stats().live_objects == g_mm_baseline_objects && kheap::check();
        if (!balanced) __atomic_fetch_add(&g_mm_errors, 1u, __ATOMIC_SEQ_CST);
        u32 errors = g_mm_errors;
        serial::printf("[smp-mm] %lu workers ran %lu heap and page operations concurrently, heap %s: %u error(s)\n",
                        MM_WORKERS, g_mm_operations, balanced ? "balanced" : "UNBALANCED", errors);
        serial::printf("[selftest] smp-mm: %s\n", errors == 0 ? "ok" : "MISMATCH");
    }
    orbital::exit_current();
}

constexpr u64 BD_WORKERS = 4;
constexpr u64 BD_SECTORS = 4;
constexpr u32 BD_ROUNDS = 40;
static bool g_bd_enabled = false;
static u64 g_bd_base = 0;
static volatile u32 g_bd_remaining = BD_WORKERS;
static volatile u32 g_bd_errors = 0;

static void bd_fail(const char* why, u64 w, u32 round) {
    __atomic_fetch_add(&g_bd_errors, 1u, __ATOMIC_SEQ_CST);
    serial::printf("[smp-blockdev] FAIL: %s (worker %lu, round %u)\n", why, w, round);
}

static void demo_blockdev_stress(void* arg) {
    u64 w = reinterpret_cast<u64>(arg);
    u8 out[BD_SECTORS * 512];
    u8 in[BD_SECTORS * 512];
    u64 start = g_bd_base + w * BD_SECTORS;
    for (u32 round = 0; round < BD_ROUNDS; ++round) {
        for (u64 i = 0; i < sizeof(out); ++i)
            out[i] = static_cast<u8>(((i * 2654435761u) >> 24) ^ (w * 37) ^ (round * 11));
        if (!blockdev::write_sectors(start, BD_SECTORS, out)) { bd_fail("batched write failed", w, round); continue; }
        for (u64 i = 0; i < sizeof(in); ++i) in[i] = 0xA5;
        if (!blockdev::read_sectors(start, BD_SECTORS, in)) { bd_fail("batched read failed", w, round); continue; }
        bool same = true;
        for (u64 i = 0; same && i < sizeof(in); ++i) same = in[i] == out[i];
        if (!same) bd_fail("batched readback mismatch", w, round);

        u8 one_out[512], one_in[512];
        for (u64 i = 0; i < 512; ++i) one_out[i] = static_cast<u8>(i + w * 53 + round * 7);
        u64 sec = start + (round % BD_SECTORS);
        if (!blockdev::write_sector(sec, one_out) || !blockdev::read_sector(sec, one_in)) { bd_fail("single sector I/O failed", w, round); continue; }
        same = true;
        for (u64 i = 0; same && i < 512; ++i) same = one_in[i] == one_out[i];
        if (!same) bd_fail("single sector mismatch", w, round);
        if ((round % 8) == 0) orbital::yield();
    }
    if (__atomic_sub_fetch(&g_bd_remaining, 1u, __ATOMIC_SEQ_CST) == 0) {
        u32 errors = g_bd_errors;
        serial::printf("[smp-blockdev] %lu workers ran %u rounds of batched and single-sector I/O on private regions: %u error(s)\n",
                        BD_WORKERS, BD_ROUNDS, errors);
        serial::printf("[selftest] smp-blockdev: %s\n", errors == 0 ? "ok" : "MISMATCH");
    }
    orbital::exit_current();
}

struct PurgeList {
    char names[8][stellar::NAME_MAX_LEN + 1];
    u64 stars[8];
    u32 types[8];
    u32 n;
};

static void purge_collect(const char* name, u64 star, u32 type, void* ctx) {
    auto* p = static_cast<PurgeList*>(ctx);
    if (p->n >= 8) return;
    u32 i = 0;
    for (; name[i] && i < stellar::NAME_MAX_LEN; ++i) p->names[p->n][i] = name[i];
    p->names[p->n][i] = 0;
    p->stars[p->n] = star;
    p->types[p->n] = type;
    ++p->n;
}

static void purge_dir(u64 dir, u32 depth) {
    if (depth > 4) return;
    for (u32 pass = 0; pass < 64; ++pass) {
        PurgeList pl;
        pl.n = 0;
        stellar::list(dir, &purge_collect, &pl);
        if (pl.n == 0) return;
        for (u32 i = 0; i < pl.n; ++i) {
            if (pl.types[i] == stellar::TYPE_CONSTELLATION) purge_dir(pl.stars[i], depth + 1);
            stellar::unlink(dir, pl.names[i]);
        }
    }
}

static bool selftest_rename() {
    using stellar::Status;
    const u64 NONE = stellar::INVALID_STAR;
    const u64 root = stellar::ROOT_STAR;
    u64 rn = stellar::find(root, "rn-selftest");
    if (rn != NONE) purge_dir(rn, 0);
    else rn = stellar::create_constellation(root, "rn-selftest");
    if (rn == NONE) return false;

    Status why = Status::Ok;
    u8 sink[8];
    u64 f = stellar::create_file(rn, "a", "one", 3);
    u64 d1 = stellar::create_constellation(rn, "d1");
    u64 d2 = stellar::create_constellation(d1, "d2");
    bool ok = f != NONE && d1 != NONE && d2 != NONE;

    ok = ok && stellar::rename(rn, "a", d1, "b");
    ok = ok && stellar::find(d1, "b") == f && stellar::find(rn, "a") == NONE && file_equals(f, "one");

    u64 g = stellar::create_file(rn, "c", "two", 3);
    ok = ok && g != NONE && stellar::rename(d1, "b", rn, "c");
    ok = ok && stellar::find(rn, "c") == f && file_equals(f, "one") && stellar::read_file(g, sink, sizeof(sink)) == stellar::READ_ERROR;

    ok = ok && !stellar::rename(rn, "d1", d2, "x", 0, &why) && why == Status::WouldCycle;
    ok = ok && stellar::rename(d1, "d2", rn, "d2m") && stellar::find(rn, "d2m") == d2;
    ok = ok && !stellar::rename(rn, "c", rn, "d1", stellar::RENAME_NOREPLACE, &why) && why == Status::Exists;

    purge_dir(rn, 0);
    ok = ok && stellar::unlink(root, "rn-selftest");
    stellar::CheckReport rep{};
    return ok && stellar::check(&rep, false) && rep.ok();
}

static bool selftest_extents() {
    using stellar::Status;
    const u64 NONE = stellar::INVALID_STAR;
    const u64 root = stellar::ROOT_STAR;
    static u8 pat[40960], back[40960];
    u64 dir = stellar::find(root, "ext-selftest");
    if (dir != NONE) purge_dir(dir, 0);
    else dir = stellar::create_constellation(root, "ext-selftest");
    if (dir == NONE) return false;

    for (u64 i = 0; i < sizeof(pat); ++i) pat[i] = static_cast<u8>(i * 7 + (i >> 9));
    stellar::StatInfo si{};
    stellar::test_set_max_extent_sectors(4);
    u64 f = stellar::create_file(dir, "a", pat, sizeof(pat));
    stellar::test_set_max_extent_sectors(0);
    bool ok = f != NONE && stellar::stat(f, &si) && (si.flags & stellar::FLAG_EXTENT_TABLE) && si.extents == 20;
    ok = ok && stellar::read_file(f, back, sizeof(back)) == sizeof(pat) && mem::compare(pat, back, sizeof(pat)) == 0;
    ok = ok && stellar::verify_file(f);

    for (u64 i = 0; i < 30000; ++i) pat[i] = static_cast<u8>(i * 5 + 3);
    ok = ok && stellar::write_file(f, pat, 30000) == f && stellar::stat(f, &si) && !(si.flags & stellar::FLAG_EXTENT_TABLE) && si.extents == 1;
    ok = ok && stellar::read_file(f, back, sizeof(back)) == 30000 && mem::compare(pat, back, 30000) == 0;

    purge_dir(dir, 0);
    ok = ok && stellar::unlink(root, "ext-selftest");
    stellar::CheckReport rep{};
    return ok && stellar::check(&rep, false) && rep.ok();
}

static void reclaim_task(void*) {
    constexpr u64 PATIENCE_MS = 20000;
    u64 start = clock::uptime_ms();
    while (orbital::cores_on_kernel_stacks() < g_expected_cores) {
        if (clock::uptime_ms() - start > PATIENCE_MS) {
            serial::printf("[universe] only %u of %lu core(s) left their bootloader stacks in %lu ms, leaving bootloader memory alone\n",
                            orbital::cores_on_kernel_stacks(), g_expected_cores, PATIENCE_MS);
            orbital::exit_current();
        }
        orbital::sleep_ms(2);
    }
    universe::reclaim_bootloader_regions(g_entries, g_n_entries);
    orbital::exit_current();
}

extern "C" NORETURN void kernel_main() {
    serial::init();
    serial::writeln("\n=== Aphelion booting ===");

    if (!LIMINE_BASE_REVISION_SUPPORTED) {
        serial::writeln("[boot] FATAL: Limine base revision unsupported");
        cpu::hang();
    }

    if (!fb_request.response || fb_request.response->framebuffer_count < 1)
        panic_no_framebuffer();

    limine_framebuffer* lfb = fb_request.response->framebuffers[0];
    fb::Info info{};
    info.base = static_cast<volatile u8*>(lfb->address);
    info.width = lfb->width; info.height = lfb->height; info.pitch = lfb->pitch;
    info.bpp = static_cast<u8>(lfb->bpp);
    info.red_shift = lfb->red_mask_shift;
    info.green_shift = lfb->green_mask_shift;
    info.blue_shift = lfb->blue_mask_shift;
    bool console_started = fb::init(info);
    if (!console_started)
        serial::writeln("[boot] the Corona console could not start (the framebuffer is not 32 bits per pixel with an aligned pitch); continuing on serial only");
    fb::clear(0x0A0A14);
    fb::printf(0x8FD3FF, "Aphelion\n");
    fb::printf(0xB0B0C0, "non-POSIX x86_64 kernel -- Corona console online\n\n");

    serial::printf("[boot] framebuffer %lux%lu @ %u bpp\n", info.width, info.height, info.bpp);

    if (!hhdm_request.response || !rsdp_request.response) {
        serial::writeln("[boot] FATAL: missing HHDM or RSDP response");
        cpu::hang();
    }
    g_hhdm_offset = hhdm_request.response->offset;

    {
        const char* cmdline = cmdline_request.response ? cmdline_request.response->cmdline : nullptr;
        bootcfg::set(bootcfg::parse(cmdline));
        const bootcfg::Config& cfg = bootcfg::get();
        serial::printf("[bootcfg] mode=%s storage=%s timer=%s%s\n", bootcfg::name(cfg.mode), bootcfg::name(cfg.storage),
                        bootcfg::name(cfg.timer), cfg.rejected ? " (some words were ignored)" : "");
    }

    percore::early_init();
    fpu::init_core();
    init_gdt_idt(true);
    fb::printf(0xC0FFC0, "[ok] GDT + TSS installed (per-core IST stacks for #DF/NMI)\n");
    fb::printf(0xC0FFC0, "[ok] IDT installed (256 vectors, no legacy PIC in the picture)\n");
    u64 early_apic_phys = apic::probe_mmio_phys_base();

    if (!memmap_request.response) {
        serial::writeln("[boot] FATAL: no memory map");
        cpu::hang();
    }
    auto* mm_resp = memmap_request.response;
    universe::MemmapEntry* entries = g_entries;
    if (mm_resp->entry_count > MAX_MEMMAP_ENTRIES) {
        serial::printf("[boot] FATAL: the bootloader reported %lu memory map entries, this kernel keeps at most %lu\n",
                        static_cast<u64>(mm_resp->entry_count), MAX_MEMMAP_ENTRIES);
        cpu::hang();
    }
    u64 n_entries = mm_resp->entry_count;
    g_n_entries = n_entries;
    for (u64 i = 0; i < n_entries; ++i)
        entries[i] = { mm_resp->entries[i]->base, mm_resp->entries[i]->length, mm_resp->entries[i]->type };
    universe::init(entries, n_entries, g_hhdm_offset);
    bool pmm_ok = universe::selftest();
    serial::printf("[selftest] pmm: %s\n", pmm_ok ? "ok" : "MISMATCH");
    fb::printf(0xC0FFC0, "[ok] Universe online: %lu galaxies, %lu MiB free\n",
               static_cast<u64>(universe::galaxy_count()), universe::free_bytes() / (1024 * 1024));

    if (!kaddr_request.response) {
        serial::writeln("[boot] FATAL: no kernel address response");
        cpu::hang();
    }
    u64 kphys = kaddr_request.response->physical_base;
    u64 kvirt = kaddr_request.response->virtual_base;
    constellation::KernelImage image{
        kphys, kvirt,
        reinterpret_cast<u64>(__requests_start), reinterpret_cast<u64>(__requests_end),
        reinterpret_cast<u64>(__text_start), reinterpret_cast<u64>(__text_end),
        reinterpret_cast<u64>(__rodata_start), reinterpret_cast<u64>(__rodata_end),
        reinterpret_cast<u64>(__data_start), reinterpret_cast<u64>(__data_end)
    };
    constellation::init(g_hhdm_offset, entries, n_entries, image);

    u64 fb_phys = (reinterpret_cast<u64>(lfb->address) - g_hhdm_offset) & ~0xFFFull;
    u64 fb_end = (reinterpret_cast<u64>(lfb->address) - g_hhdm_offset + lfb->pitch * lfb->height + 0xFFF) & ~0xFFFull;
    u64 fb_flags = constellation::WRITABLE | constellation::NO_EXECUTE;
    if (percore::wc_enabled()) fb_flags |= constellation::WRITE_COMBINING;
    if (!constellation::map_range(g_hhdm_offset + fb_phys, fb_phys, fb_end - fb_phys, fb_flags)) {
        serial::writeln("[boot] FATAL: could not map the framebuffer");
        cpu::hang();
    }
    fb::printf(0xC0FFC0, "[ok] The Core constellation is live (own page tables, CR3 switched)\n");

    map_device_page_or_die(early_apic_phys, "local APIC");
    bool ist_ok = gdt::guard_ist_stacks() && gdt::ist_guards_ok();
    serial::printf("[selftest] ist guards: %s\n", ist_ok ? "ok" : "MISMATCH");
    bool vmm_ok = constellation::selftest();
    serial::printf("[selftest] vmm: %s\n", vmm_ok ? "ok" : "MISMATCH");

    kheap::init();
    bool heap_ok = kheap::selftest();
    serial::printf("[selftest] kheap: %s\n", heap_ok ? "ok" : "MISMATCH");
    fb::printf(0xC0FFC0, "[ok] Kernel heap online: slab classes 16..%lu B, large blocks from Universe\n",
               static_cast<u64>(kheap::MAX_SMALL));

    if (console_started) {
        bool buffered = fb::enable_backbuffer();
        corona::Metrics cm = corona::metrics();
        serial::printf("[console] Corona %lux%lu cells of %lux%lu px on a %lux%lu framebuffer, %s\n",
                        static_cast<u64>(cm.cols), static_cast<u64>(cm.rows), static_cast<u64>(cm.cell_w), static_cast<u64>(cm.cell_h),
                        cm.fb_width, cm.fb_height,
                        buffered ? "RAM backbuffer on (the write-combined framebuffer is only ever written)" : "no backbuffer, drawing straight to the framebuffer");
        bool console_ok = corona::selftest();
        serial::printf("[selftest] console: %s\n", console_ok ? "ok" : "MISMATCH");
    }

    apic::init(g_hhdm_offset);
    if (apic::using_x2apic()) {
        fb::printf(0xC0FFC0, "[ok] x2APIC online (MSR-based), id=%d\n", static_cast<int>(apic::id()));
        serial::printf("[apic] BSP: x2APIC mode (MSR-based), id=%d\n", static_cast<int>(apic::id()));
    } else {
        fb::printf(0xE0D080, "[ok] xAPIC online (MMIO fallback -- this CPU/hypervisor has no x2APIC), id=%d\n",
                    static_cast<int>(apic::id()));
        serial::printf("[apic] BSP: xAPIC mode (MMIO fallback, no x2APIC support), id=%d\n",
                        static_cast<int>(apic::id()));
    }

    tlb::init_core();
    acpi::init(reinterpret_cast<u64>(rsdp_request.response->address));
    fb::printf(0xC0FFC0, "[ok] ACPI/MADT parsed: %d CPU(s) reported\n",
               static_cast<int>(acpi::info().cpu_count));
    {
        const acpi::Info& ai = acpi::info();
        if (!ai.fadt_found) {
            serial::writeln("[acpi] no FADT found: no reset register and no IAPC boot flags");
        } else {
            serial::printf("[acpi] FADT revision %u, reset register %s", static_cast<unsigned>(ai.fadt_revision),
                            ai.reset_supported ? "present" : "absent");
            if (ai.iapc_known)
                serial::printf(", IAPC boot flags %x: 8042 %s, legacy devices %s, CMOS RTC %s\n", static_cast<unsigned>(ai.iapc_boot_arch),
                                (ai.iapc_boot_arch & acpi::IAPC_8042) ? "present" : "declared absent",
                                (ai.iapc_boot_arch & acpi::IAPC_LEGACY_DEVICES) ? "present" : "declared absent",
                                (ai.iapc_boot_arch & acpi::IAPC_NO_CMOS_RTC) ? "absent" : "present");
            else
                serial::writeln(", IAPC boot flags not provided by this FADT revision");
        }
    }

    timesrc::init(acpi::info(), g_hhdm_offset, bootcfg::get().timer);
    clock::init(apic::id());
    stellar::set_clock(&clock::now_ms);
    if (clock::wall_valid()) {
        char iso[21];
        civil::format_iso(civil::from_unix(clock::boot_wall_ms() / 1000), iso);
        fb::printf(0xC0FFC0, "[ok] RTC clock: %s, file mtimes are Unix milliseconds\n", iso);
    } else {
        fb::printf(0xE0D080, "[--] RTC unreadable; file mtimes stay 0\n");
    }

    input::init();
    bool input_ok = input::selftest();
    serial::printf("[selftest] input: %s\n", input_ok ? "ok" : "MISMATCH");

    keyboard::Status kb_status = keyboard::init(idt::VEC_KEYBOARD);
    serial::printf("[keyboard] i8042: %s, controller config %x\n", keyboard::status_name(kb_status),
                    static_cast<unsigned>(keyboard::config_byte()));
    bool kb_irq = false;
    bool serial_irq = false;
    if (acpi::info().ioapic_found) {
        map_device_page_or_die(acpi::info().ioapic_base, "IOAPIC");
        ioapic::init(g_hhdm_offset, acpi::info().ioapic_base, acpi::info().ioapic_gsi_base);
        if (kb_status == keyboard::Status::Ready) {
            acpi::Redirection kbd_irq = acpi::resolve_isa_irq(1);
            ioapic::set_redirection(kbd_irq.gsi, idt::VEC_KEYBOARD, apic::id(), false,
                                     kbd_irq.active_low, kbd_irq.level_triggered);
            keyboard::set_irq_routed(true);
            kb_irq = true;
        }
        if (serial::present() && serial::enable_rx_irq(idt::VEC_SERIAL)) {
            acpi::Redirection com1_irq = acpi::resolve_isa_irq(4);
            ioapic::set_redirection(com1_irq.gsi, idt::VEC_SERIAL, apic::id(), false,
                                     com1_irq.active_low, com1_irq.level_triggered);
            serial_irq = true;
        }
        fb::printf(0xC0FFC0, "[ok] IOAPIC online (%d redirection entries)\n", static_cast<int>(ioapic::max_redirection_entries()));
    } else {
        fb::printf(0xE0D080, "[--] No IOAPIC reported; keyboard and serial input fall back to polling\n");
    }
    if (kb_status == keyboard::Status::Ready)
        fb::printf(0xC0FFC0, "[ok] PS/2 keyboard %s\n", kb_irq ? "on IRQ1 -- try typing" : "polled (no IOAPIC)");
    else
        fb::printf(0xE0D080, "[--] No PS/2 keyboard controller: %s\n", keyboard::status_name(kb_status));
    serial::printf("[input] keyboard %s, serial %s\n", kb_irq ? "interrupt-driven" : (kb_status == keyboard::Status::Ready ? "polled" : "unavailable"),
                    serial::present() ? (serial_irq ? "interrupt-driven" : "polled") : "absent (no UART answered)");

    const bool test_mode = bootcfg::get().mode == bootcfg::Mode::Test;

    const bool usb_mode = bootcfg::get().storage == bootcfg::Storage::Usb;

    if (bootcfg::get().storage == bootcfg::Storage::Auto) {
        virtioblk::init(g_hhdm_offset);
        ahci::init(g_hhdm_offset);
        nvme::init(g_hhdm_offset);
    } else if (usb_mode) {
        start_usb_storage();
    } else {
        serial::writeln("[storage] disabled by the boot configuration: no disk driver is started and no disk is touched");
        fb::printf(0xE0D080, "[--] Storage disabled by the boot configuration: no disk is initialised or touched\n");
    }
    nebula::register_builtin_realms();
    if (!lens::mint_boot_roots().ok()) serial::writeln("[lens] FATAL: could not mint the boot root lenses");
    if (!lens::mint_boot_console(lens::console_stream()).ok()) serial::writeln("[lens] FATAL: could not mint the console lens");
    input::set_wake_hook(&lens::stream_event);
    lens::seal();
    nebula::disk_set_confirmed_only(usb_mode);
    if (blockdev::present() && (!test_mode || usb_mode)) {
        fb::printf(0xC0FFC0, "[ok] %s online (%lu sectors)\n", blockdev::active()->name(), blockdev::capacity_sectors());
        serial::printf("[storage] %s found; the filesystem test suite is skipped (%s)\n", blockdev::active()->name(),
                        usb_mode ? "the USB window is never formatted or written without an explicit command" : "shell mode");
        stellar::init(g_hhdm_offset);
        stellar::Status shell_mount_why = stellar::Status::Ok;
        if (stellar::mount(&shell_mount_why)) {
            nebula::disk_set_mounted(true);
            set_disk_note(usb_mode ? "USB window: Stellar volume mounted as disk:" : "Stellar volume mounted as disk:", nullptr);
            serial::printf("[nebula] Stellar volume mounted for the shell: %lu sectors, %lu free\n", stellar::total_sectors(), stellar::free_space_sectors());
        } else {
            const char* why_text = usb_mode && shell_mount_why == stellar::Status::NotFormatted ? "blank, not formatted yet" : stellar::status_name(shell_mount_why);
            set_disk_note(usb_mode ? "USB window has no Stellar volume: " : "no Stellar volume: ", why_text);
            serial::printf("[nebula] no Stellar volume for the shell (%s); %s\n", why_text,
                            usb_mode ? "storage init erase <MiB> creates one in the USB window" : "mkfs erase creates one on a blank disk");
        }
        if (usb_mode) {
            serial::printf("[storage] %s\n", g_disk_note);
            fb::printf(nebula::disk_is_mounted() ? 0xC0FFC0 : 0xE0D080, "[%s] %s\n", nebula::disk_is_mounted() ? "ok" : "--", g_disk_note);
        }
    } else if (blockdev::present()) {
        fb::printf(0xC0FFC0, "[ok] %s online (%lu sectors)\n",
                   blockdev::active()->name(), blockdev::capacity_sectors());

        stellar::init(g_hhdm_offset);
        stellar::Status mount_why = stellar::Status::Ok;
        bool fs_ready = stellar::mount(&mount_why);
        bool fresh_fs = false;
        if (!fs_ready) {
            if (mount_why == stellar::Status::NotFormatted && stellar::can_auto_format()) {
                serial::writeln("[stellar] blank disk: formatting");
                u64 capacity = blockdev::capacity_sectors();
                u64 fs_sectors = capacity > TEST_TAIL_SECTORS + 64 ? capacity - TEST_TAIL_SECTORS : capacity;
                fs_ready = stellar::format(fs_sectors);
                fresh_fs = fs_ready;
            } else {
                serial::printf("[stellar] refusing to format: mount failed (%s)%s\n", stellar::status_name(mount_why),
                                mount_why == stellar::Status::NotFormatted ? ", and the disk holds data that is not a Stellar volume" : "");
                fb::printf(0xE0D080, "[--] Stellar FS: mount failed (%s), disk left untouched\n", stellar::status_name(mount_why));
            }
        }
        bool tail_free = fs_ready && stellar::total_sectors() + TEST_TAIL_SECTORS <= blockdev::capacity_sectors();
        nebula::disk_set_mounted(fs_ready);

        u64 total_sectors = blockdev::capacity_sectors();
        if (tail_free && total_sectors > 64) {
            constexpr u64 SCRATCH_COUNT = 32;
            u64 scratch_start = total_sectors - SCRATCH_COUNT;
            static u8 scratch_write[SCRATCH_COUNT * 512];
            static u8 scratch_read[SCRATCH_COUNT * 512];
            static u8 scratch_saved[SCRATCH_COUNT * 512];
            bool saved_ok = blockdev::read_sectors(scratch_start, SCRATCH_COUNT, scratch_saved);
            for (u64 i = 0; i < sizeof(scratch_write); ++i)
                scratch_write[i] = static_cast<u8>((i * 2654435761u) >> 24);
            bool batch_write_ok = saved_ok && blockdev::write_sectors(scratch_start, SCRATCH_COUNT, scratch_write);
            bool batch_read_ok = batch_write_ok && blockdev::read_sectors(scratch_start, SCRATCH_COUNT, scratch_read);
            bool batch_match = batch_write_ok && batch_read_ok;
            for (u64 i = 0; batch_match && i < sizeof(scratch_write); ++i)
                batch_match = (scratch_write[i] == scratch_read[i]);
            if (batch_write_ok) blockdev::write_sectors(scratch_start, SCRATCH_COUNT, scratch_saved);
            fb::printf(batch_match ? 0xC0FFC0 : 0xE0D080,
                       "[%s] %s multi-request queueing: %lu sectors written+read in one batch, %s\n",
                       batch_match ? "ok" : "--", blockdev::active()->name(), SCRATCH_COUNT,
                       batch_match ? "byte-for-byte match" : "MISMATCH");
            serial::printf("[selftest] multi-request queueing: %s\n", batch_match ? "ok" : "MISMATCH");
        }

        {
            bool flush_ok = blockdev::flush();
            fb::printf(flush_ok ? 0xC0FFC0 : 0xE0D080, "[%s] %s write barrier (flush) completed\n",
                       flush_ok ? "ok" : "--", blockdev::active()->name());
            serial::printf("[selftest] flush: %s\n", flush_ok ? "ok" : "MISMATCH");
        }

        if (fs_ready) {
            const char* msg = "Aphelion Stellar FS -- real file, real disk, real bytes.\n";
            u64 msg_len = 0;
            while (msg[msg_len]) ++msg_len;

            u64 file = stellar::find(stellar::ROOT_STAR, "hello.txt");
            if (file == stellar::INVALID_STAR)
                file = stellar::create_file(stellar::ROOT_STAR, "hello.txt", msg, msg_len);

            u64 subdir = stellar::find(stellar::ROOT_STAR, "sub");
            if (subdir == stellar::INVALID_STAR)
                subdir = stellar::create_constellation(stellar::ROOT_STAR, "sub");
            const char* nested_msg = "nested constellation works\n";
            u64 nested_len = 0;
            while (nested_msg[nested_len]) ++nested_len;
            u64 nested_file = stellar::find(subdir, "nested.txt");
            if (nested_file == stellar::INVALID_STAR)
                nested_file = stellar::create_file(subdir, "nested.txt", nested_msg, nested_len);
            static u8 nested_readback[64];
            u64 nested_n = stellar::read_file(nested_file, nested_readback, sizeof(nested_readback) - 1);
            if (nested_n == stellar::READ_ERROR) nested_n = 0;
            nested_readback[nested_n] = 0;
            serial::printf("[stellar] /sub/nested.txt (%lu bytes): %s\n",
                            nested_n, reinterpret_cast<const char*>(nested_readback));

            {
                using stellar::Status;
                Status st = Status::Internal;
                u8 probe[16];
                bool resolve_ok = stellar::resolve("/sub/nested.txt", stellar::LIVE, &st) == nested_file && st == Status::Ok;
                bool missing_ok = stellar::find(stellar::ROOT_STAR, "no-such-file", stellar::LIVE, &st) == stellar::INVALID_STAR &&
                                  st == Status::NotFound;
                bool dup_ok = stellar::create_file(stellar::ROOT_STAR, "hello.txt", "x", 1, &st) == stellar::INVALID_STAR &&
                              st == Status::Exists;
                bool name_ok = stellar::create_file(stellar::ROOT_STAR, "bad/name", "x", 1, &st) == stellar::INVALID_STAR &&
                               st == Status::InvalidName;
                bool dir_ok = stellar::read_file(subdir, probe, sizeof(probe), stellar::LIVE, &st) == stellar::READ_ERROR &&
                              st == Status::IsADirectory;
                bool api_ok = resolve_ok && missing_ok && dup_ok && name_ok && dir_ok;
                fb::printf(api_ok ? 0xC0FFC0 : 0xE0D080,
                           "[%s] Stellar FS API: path resolution, duplicate/invalid-name/not-found/is-a-directory status codes, %s\n",
                           api_ok ? "ok" : "--", api_ok ? "all as expected" : "MISMATCH");
                serial::printf("[selftest] stellar api: %s\n", api_ok ? "ok" : "MISMATCH");
            }

            {
                constexpr u64 PLAUSIBLE_MIN_MS = 1704067200000ull;
                u64 probe = stellar::find(subdir, "clock.probe");
                if (probe == stellar::INVALID_STAR)
                    probe = stellar::create_file(subdir, "clock.probe", "t", 1);
                stellar::StatInfo probe_info{};
                bool probe_ok = probe != stellar::INVALID_STAR && stellar::write_file(probe, "t", 1) == probe &&
                                stellar::stat(probe, &probe_info);
                if (!clock::wall_valid()) {
                    serial::printf("[selftest] clock mtime: skipped, no wall clock\n");
                } else {
                    probe_ok = probe_ok && probe_info.mtime >= PLAUSIBLE_MIN_MS && probe_info.mtime >= clock::boot_wall_ms();
                    char iso[21];
                    civil::format_iso(civil::from_unix(probe_info.mtime / 1000), iso);
                    serial::printf("[clock] /sub/clock.probe mtime %s (%lu ms)\n", iso, probe_info.mtime);
                    serial::printf("[selftest] clock mtime: %s\n", probe_ok ? "ok" : "MISMATCH");
                }
            }

            {
                bool extents_ok = selftest_extents();
                fb::printf(extents_ok ? 0xC0FFC0 : 0xE0D080,
                           "[%s] Stellar FS extent tables: a 20-extent file written and read through the block driver, rewritten contiguous, %s\n",
                           extents_ok ? "ok" : "--", extents_ok ? "all as expected" : "MISMATCH");
                serial::printf("[selftest] extents: %s\n", extents_ok ? "ok" : "MISMATCH");
            }

            {
                bool rename_ok = selftest_rename();
                fb::printf(rename_ok ? 0xC0FFC0 : 0xE0D080,
                           "[%s] Stellar FS rename: move, replace, NOREPLACE and directory-cycle refusal, %s\n",
                           rename_ok ? "ok" : "--", rename_ok ? "all as expected" : "MISMATCH");
                serial::printf("[selftest] rename: %s\n", rename_ok ? "ok" : "MISMATCH");
            }

            stellar::BitmapSummary bs = stellar::bitmap_summary();
            serial::printf("[stellar] free-space bitmap: %lu KiB resident, %lu of %lu summary pages loaded so far (the rest load on first use), %lu sectors on the volume\n",
                            stellar::bitmap_ram_bytes() / 1024, bs.known_pages, bs.pages, stellar::total_sectors());

            static u8 readback[128];
            u64 n = stellar::read_file(file, readback, sizeof(readback) - 1);
            if (n == stellar::READ_ERROR) n = 0;
            readback[n] = 0;

            bool match = file_equals(file, msg) || file_equals(file, HELLO_REWRITTEN);

            fb::printf(match ? 0xC0FFC0 : 0xE0D080,
                       "[%s] Stellar FS: %s /hello.txt, read %lu bytes back, %s\n",
                       match ? "ok" : "--", fresh_fs ? "created" : "found", n, match ? "matched exactly" : "MISMATCH");
            serial::printf("[stellar] readback: %s\n", reinterpret_cast<const char*>(readback));
            serial::printf("[selftest] hello.txt: %s\n", match ? "ok" : "MISMATCH");
            if (virtioblk::present())
                serial::printf("[virtio-blk] completion mode: %s, %u interrupt(s) delivered\n",
                                virtioblk::using_msix() ? "MSI-X" : "polled", virtioblk::irq_count());
            if (ahci::present())
                serial::printf("[ahci] completion mode: %s, %u interrupt(s) delivered\n",
                                ahci::using_msi() ? "MSI" : "polled", ahci::irq_count());
            if (nvme::present())
                serial::printf("[nvme] completion mode: %s, %u interrupt(s) delivered\n",
                                nvme::completion_mode(), nvme::irq_count());

            if (fresh_fs) {
                const char* rewritten_msg = HELLO_REWRITTEN;
                u64 rewritten_len = cstr_len(rewritten_msg);

                bool cow_pre_ok = stellar::verify_file(file);
                stellar::IoStats io_before = stellar::io_stats();
                u64 snap = stellar::snapshot();
                stellar::IoStats io_after = stellar::io_stats();
                u64 snap_writes = io_after.writes - io_before.writes;
                u64 snap_reads = io_after.reads - io_before.reads;

                u64 rewritten_star = stellar::write_file(file, rewritten_msg, rewritten_len);
                bool cow_write_ok = (rewritten_star == file) && stellar::verify_file(file);
                bool cow_snap_ok = (snap != stellar::INVALID_STAR) && stellar::verify_file(file, snap);
                bool snap_content_ok = file_equals(file, msg, snap);
                bool new_content_ok = file_equals(file, rewritten_msg);
                bool cow_ok = cow_pre_ok && cow_write_ok && cow_snap_ok && snap_content_ok && new_content_ok;
                fb::printf(cow_ok ? 0xC0FFC0 : 0xE0D080,
                           "[%s] Stellar FS COW: snapshot %lu keeps the pre-write bytes, live view carries the rewrite, both checksums verify\n",
                           cow_ok ? "ok" : "--", snap);

                u64 live = stellar::create_constellation(stellar::ROOT_STAR, "tree");
                u64 live_a = stellar::create_file(live, "a.txt", "one\n", 4);
                u64 live_sub = stellar::create_constellation(live, "sub");
                u64 live_n = stellar::create_file(live_sub, "n.txt", "nested\n", 7);

                io_before = stellar::io_stats();
                u64 tree_snap = stellar::snapshot();
                io_after = stellar::io_stats();
                bool o1_ok = (io_after.writes - io_before.writes <= 6) && (io_after.reads - io_before.reads == 0) &&
                             snap_writes <= 6 && snap_reads == 0;

                stellar::write_file(live_a, "two two\n", 8);
                u64 live_b = stellar::create_file(live, "b.txt", "new\n", 4);

                bool tree_ok = tree_snap != stellar::INVALID_STAR &&
                               stellar::find(live, "a.txt", tree_snap) == live_a &&
                               file_equals(live_a, "one\n", tree_snap) && stellar::verify_file(live_a, tree_snap) &&
                               file_equals(live_a, "two two\n") && stellar::verify_file(live_a) &&
                               stellar::find(live, "sub", tree_snap) == live_sub &&
                               file_equals(live_n, "nested\n", tree_snap) &&
                               stellar::find(live, "b.txt", tree_snap) == stellar::INVALID_STAR &&
                               stellar::find(live, "b.txt") == live_b;
                fb::printf(tree_ok ? 0xC0FFC0 : 0xE0D080,
                           "[%s] Stellar FS tree snapshot %lu froze a.txt + sub/n.txt while the live tree diverged (rewrite + new file), %s\n",
                           tree_ok ? "ok" : "--", tree_snap, tree_ok ? "isolation confirmed" : "FAILED");
                fb::printf(o1_ok ? 0xC0FFC0 : 0xE0D080,
                           "[%s] Stellar FS snapshot cost: %lu sector write(s), %lu read(s) -- independent of tree size\n",
                           o1_ok ? "ok" : "--", io_after.writes - io_before.writes, io_after.reads - io_before.reads);
                serial::printf("[stellar] snapshots: %u in table, snapshot() cost %lu write(s) + %lu read(s) each (O(1), epoch-based)\n",
                                stellar::snapshot_count(), io_after.writes - io_before.writes, io_after.reads - io_before.reads);
                serial::printf("[selftest] COW and tree snapshots: %s\n", (cow_ok && tree_ok && o1_ok) ? "ok" : "MISMATCH");
            } else {
                serial::printf("[stellar] snapshot self-tests skipped on a mounted disk (%u/%u snapshot slots in use); delete disk.img to rerun them\n",
                                stellar::snapshot_count(), stellar::SNAPSHOT_MAX);
            }

            constexpr u64 BULK_BYTES = 8192;
            static u8 bulk_write[BULK_BYTES];
            static u8 bulk_read[BULK_BYTES];
            for (u64 i = 0; i < BULK_BYTES; ++i) bulk_write[i] = static_cast<u8>((i * 40503u) >> 8);
            u64 bulk = stellar::find(stellar::ROOT_STAR, "bulk.bin");
            if (bulk == stellar::INVALID_STAR)
                bulk = stellar::create_file(stellar::ROOT_STAR, "bulk.bin", bulk_write, BULK_BYTES);
            u64 bulk_n = stellar::read_file(bulk, bulk_read, BULK_BYTES);
            bool bulk_ok = (bulk_n == BULK_BYTES) && stellar::verify_file(bulk);
            for (u64 i = 0; bulk_ok && i < BULK_BYTES; ++i) bulk_ok = (bulk_read[i] == bulk_write[i]);
            fb::printf(bulk_ok ? 0xC0FFC0 : 0xE0D080,
                       "[%s] Stellar FS bulk I/O: /bulk.bin %lu bytes through the batched block path, checksum verified, %s\n",
                       bulk_ok ? "ok" : "--", bulk_n, bulk_ok ? "byte-for-byte match" : "MISMATCH");
            serial::printf("[selftest] bulk I/O: %s\n", bulk_ok ? "ok" : "MISMATCH");

            constexpr u64 STRESS_COUNT = 14;
            static u64 stress_ids[STRESS_COUNT];
            bool stress_create_ok = true;

            u64 stale_stress = stellar::find(stellar::ROOT_STAR, "smpstress");
            if (stale_stress != stellar::INVALID_STAR) {
                purge_dir(stale_stress, 0);
                stellar::unlink(stellar::ROOT_STAR, "smpstress");
                serial::writeln("[stellar] removed the smpstress directory left behind by an interrupted boot");
            }
            for (u64 i = 0; i < STRESS_COUNT; ++i) {
                char digits[20];
                u32 dlen = itoa_dec(i, digits);
                char name[32] = "stress";
                for (u32 j = 0; j < dlen; ++j) name[6 + j] = digits[j];
                name[6 + dlen] = 0;
                stress_ids[i] = stellar::find(stellar::ROOT_STAR, name);
                if (stress_ids[i] == stellar::INVALID_STAR)
                    stress_ids[i] = stellar::create_file(stellar::ROOT_STAR, name, digits, dlen);
                if (stress_ids[i] == stellar::INVALID_STAR) stress_create_ok = false;
            }

            u64 stress_mismatches = 0;
            for (u64 i = 0; i < STRESS_COUNT; ++i) {
                char expect[20];
                u32 elen = itoa_dec(i, expect);
                u8 got[20];
                u64 n = stellar::read_file(stress_ids[i], got, sizeof(got) - 1);
                bool ok = (n == elen);
                for (u64 j = 0; ok && j < n; ++j) ok = (got[j] == static_cast<u8>(expect[j]));
                if (!ok) ++stress_mismatches;
            }

            u64 dir_count = 0;
            stellar::list(stellar::ROOT_STAR, &count_entry, &dir_count);
            u64 expected_dir_count = STRESS_COUNT;
            const char* const known_names[4] = { "hello.txt", "sub", "tree", "bulk.bin" };
            for (const char* known : known_names)
                if (stellar::find(stellar::ROOT_STAR, known) != stellar::INVALID_STAR) ++expected_dir_count;

            bool stress_ok = stress_create_ok && (stress_mismatches == 0) && (dir_count == expected_dir_count);
            if (stress_ok) {
                g_fs_stress_dir = stellar::find(stellar::ROOT_STAR, "smpstress");
                if (g_fs_stress_dir == stellar::INVALID_STAR)
                    g_fs_stress_dir = stellar::create_constellation(stellar::ROOT_STAR, "smpstress");
                if (g_fs_stress_dir != stellar::INVALID_STAR) {
                    g_fs_stress_alt = stellar::create_constellation(g_fs_stress_dir, "alt");
                    u64 shared = stellar::create_file(g_fs_stress_dir, "shared", "shared", 6);
                    g_fs_stress_enabled = g_fs_stress_alt != stellar::INVALID_STAR && shared != stellar::INVALID_STAR;
                }
            }
            fb::printf(stress_ok ? 0xC0FFC0 : 0xE0D080,
                       "[%s] Stellar FS B+tree + growable directory stress: %lu stars, %lu mismatch(es), %lu dir entries enumerated\n",
                       stress_ok ? "ok" : "--", STRESS_COUNT, stress_mismatches, dir_count);
            serial::printf("[stellar] stress: %lu stars created (create_ok=%d), %lu mismatch(es), root dir enumerates %lu/%lu entries across its B+tree\n",
                            STRESS_COUNT, stress_create_ok ? 1 : 0, stress_mismatches, dir_count, expected_dir_count);
        } else {
            fb::printf(0xE0D080, "[--] Stellar FS: not available, see the serial log\n");
        }
    } else {
        fb::printf(0xB0B0C0, "[--] No block device found\n");
    }

    static u32 core_ids[gdt::MAX_CORES];
    u32 core_id_count = 0;
    if (smp_request.response) {
        for (u64 i = 0; i < smp_request.response->cpu_count && core_id_count < gdt::MAX_CORES; ++i)
            core_ids[core_id_count++] = smp_request.response->cpus[i]->lapic_id;
    } else {
        core_ids[core_id_count++] = apic::id();
    }
    orbital::init(g_hhdm_offset, core_ids, core_id_count);
    bool timer_ok = timesrc::selftest();
    serial::printf("[selftest] timer: %s\n", timer_ok ? "ok" : "MISMATCH");
    bool guards_ok = orbital::selftest();
    serial::printf("[selftest] stack guards: %s\n", guards_ok ? "ok" : "MISMATCH");

    g_cpus_online = 1;
    if (smp_request.response && smp_request.response->cpu_count > 1) {
        u64 total = smp_request.response->cpu_count;
        if (total > gdt::MAX_CORES) {
            serial::printf("[smp] %lu cores reported, waking only %u (the per-core GDT/TSS limit)\n",
                            total, static_cast<u32>(gdt::MAX_CORES));
            total = gdt::MAX_CORES;
        }
        g_expected_cores = total;
        u64 woken = 0;
        for (u64 i = 0; i < smp_request.response->cpu_count && woken + 1 < total; ++i) {
            limine_smp_info* c = smp_request.response->cpus[i];
            if (c->lapic_id == smp_request.response->bsp_lapic_id) continue;
            c->extra_argument = 0;
            c->goto_address = ap_entry;
            ++woken;
        }
        fb::printf(0xC0FFC0, "[ok] SMP: waking %lu additional core(s)...\n", woken);

        for (u64 i = 0; i < 300000000ull && g_cpus_online < total; ++i) asm volatile("pause" ::: "memory");
        fb::printf(0xC0FFC0, "[ok] SMP: %lu Sun(s) online in total\n", g_cpus_online);
    } else {
        fb::printf(0xB0B0C0, "[--] SMP: single core reported, nothing else to wake\n");
    }

    fpu::report();
    bool tlb_ok = constellation::smp_selftest() && tlb::online_count() >= g_cpus_online;
    serial::printf("[tlb] %u core(s) take TLB shootdowns\n", tlb::online_count());
    serial::printf("[selftest] tlb shootdown: %s\n", tlb_ok ? "ok" : "MISMATCH");
    bool percore_ok = percore::selftest() && percore::cores_seen() >= g_cpus_online;
    serial::printf("[percore] %u core(s) verified: NX %s, WP on, WC PAT %s, SMEP %s, SMAP %s\n", percore::cores_seen(),
                    percore::nx_enabled() ? "on" : "off", percore::wc_enabled() ? "on" : "off",
                    percore::smep_enabled() ? "on" : "off", percore::smap_enabled() ? "on" : "off");
    serial::printf("[selftest] percore: %s\n", percore_ok ? "ok" : "MISMATCH");

    bool gdt_ok = gdt::selftest() && gdt::cores_seen() >= g_cpus_online;
    serial::printf("[gdt] %u core(s) verified on private GDT, TSS and IST stacks\n", gdt::cores_seen());
    serial::printf("[selftest] per-core gdt: %s\n", gdt_ok ? "ok" : "MISMATCH");

    if (test_mode) {
        bool nebula_ok = nebula::selftest();
        serial::printf("[selftest] nebula: %s\n", nebula_ok ? "ok" : "MISMATCH");
    }
    fb::printf(0x8FD3FF, "\nOrbital scheduler: bringing up this core...\n");
    clock::sync();
    orbital::init_core();
    corona::enable_locking();
    if (test_mode) {
        orbital::spawn("alpha", &demo_cooperative, const_cast<char*>("alpha"));
        orbital::spawn("beta", &demo_cooperative_explicit_exit, const_cast<char*>("beta"));
        orbital::spawn("gamma-spinner", &demo_spinner, const_cast<char*>("gamma-spinner"));
        orbital::spawn("clock-test", &demo_clock, nullptr);
        for (u64 i = 0; i < WORKER_COUNT; ++i)
            orbital::spawn("worker", &demo_worker, reinterpret_cast<void*>(i));
        u64 fs_total = stellar::total_sectors();
        if (blockdev::present() && fs_total == 0) {
            serial::writeln("[boot] block-device stress skipped: no mounted Stellar volume, so the disk tail is not ours to write");
        } else if (blockdev::present() && blockdev::capacity_sectors() > 256) {
            if (fs_total + TEST_TAIL_SECTORS <= blockdev::capacity_sectors()) {
                g_bd_base = blockdev::capacity_sectors() - TEST_TAIL_SECTORS;
                g_bd_enabled = true;
                for (u64 i = 0; i < BD_WORKERS; ++i)
                    orbital::spawn("bd-stress", &demo_blockdev_stress, reinterpret_cast<void*>(i));
            } else {
                serial::writeln("[boot] block-device stress skipped: the filesystem covers the last 64 sectors; run make reset-disk");
            }
        }
        if (g_fs_stress_enabled) {
            for (u64 i = 0; i < FS_STRESS_WORKERS; ++i)
                orbital::spawn("fs-stress", &demo_fs_stress, reinterpret_cast<void*>(i));
            fb::printf(0xC0FFC0, "[ok] %lu Satellites will hammer Stellar FS concurrently\n", FS_STRESS_WORKERS);
        }
        fb::printf(0xC0FFC0, "[ok] Orbital scheduler online -- 3 demo Satellites + %lu work-stealing workers spawned\n",
                   WORKER_COUNT);
    }
    serial::writeln("[boot] === Aphelion is up. Handing off to the Orbital scheduler. ===");
    if (test_mode) {
        g_mm_baseline_objects = kheap::stats().live_objects;
        for (u64 i = 0; i < MM_WORKERS; ++i)
            orbital::spawn("mm-stress", &demo_mm_stress, reinterpret_cast<void*>(i));
    } else {
        helm::Facts facts{};
        facts.suns = static_cast<u32>(g_cpus_online);
        facts.storage = blockdev::present() ? blockdev::active()->name()
                        : (bootcfg::get().storage == bootcfg::Storage::Off ? "disabled by the boot configuration" : "no block device found");
        facts.usb_mode = usb_mode;
        facts.keyboard_ready = kb_status == keyboard::Status::Ready;
        facts.keyboard_irq = kb_irq;
        facts.serial_present = serial::present();
        facts.serial_irq = serial_irq;
        facts.disk_mounted = nebula::disk_is_mounted();
        facts.disk_note = g_disk_note;
        helm::set_facts(facts);
        nebula::sys_set_facts(facts.storage, facts.suns);
        if (!helm::spawn()) serial::writeln("[boot] FATAL: could not start the Helm shell");
    }
    orbital::spawn("reclaim", &reclaim_task, nullptr);
    if (test_mode) orbital::spawn_selftest();
    orbital::start_core();
}
