#include <limine.h>
#include <cosmos/types.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/framebuffer.hpp>
#include <cosmos/gdt.hpp>
#include <cosmos/idt.hpp>
#include <cosmos/apic.hpp>
#include <cosmos/acpi.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/vmm.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/orbital.hpp>
#include <cosmos/ioapic.hpp>
#include <cosmos/keyboard.hpp>
#include <cosmos/pci.hpp>
#include <cosmos/virtio_blk.hpp>
#include <cosmos/ahci.hpp>
#include <cosmos/nvme.hpp>
#include <cosmos/blockdev.hpp>
#include <cosmos/stellar.hpp>
#include <cosmos/clock.hpp>
#include <cosmos/civil.hpp>

extern "C" char __kernel_start[];
extern "C" char __kernel_end[];

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

static constexpr u64 TEST_TAIL_SECTORS = 64;
static constexpr const char* HELLO_REWRITTEN = "Aphelion Stellar FS -- rewritten after snapshot.\n";

static u32 init_gdt_idt(bool is_bsp) {
    static u8 df_stack[8][4096] __attribute__((aligned(16)));
    static u8 nmi_stack[8][4096] __attribute__((aligned(16)));

    u32 core_idx = is_bsp ? 0 : static_cast<u32>(__atomic_fetch_add(&g_next_core_idx, 1, __ATOMIC_SEQ_CST));
    if (core_idx >= 8) core_idx = 7;

    gdt::init(reinterpret_cast<u64>(&df_stack[core_idx][4096]),
              reinterpret_cast<u64>(&nmi_stack[core_idx][4096]));
    idt::init();
    return core_idx;
}

static void bring_up_core(bool is_bsp) {
    init_gdt_idt(is_bsp);
    apic::init(g_hhdm_offset);
}

extern "C" void ap_entry(limine_smp_info* info) {

    cpu::write_cr3(constellation::core_pml4_phys());
    bring_up_core(false);
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
    u64 total = FS_STRESS_WORKERS * FS_STRESS_FILES;
    u64 listed = 0;
    stellar::list(g_fs_stress_dir, &count_entry, &listed);
    if (listed != total) {
        serial::printf("[smp-fs] FAIL: directory holds %lu entries, expected %lu\n", listed, total);
        __atomic_fetch_add(&g_fs_stress_errors, 1u, __ATOMIC_SEQ_CST);
    }
    for (u64 w = 0; w < FS_STRESS_WORKERS; ++w) {
        for (u64 j = 0; j < FS_STRESS_FILES; ++j) {
            char name[24];
            fs_stress_name(name, w, j);
            u64 star = stellar::find(g_fs_stress_dir, name);
            if (!file_equals(star, name) || !stellar::verify_file(star)) fs_stress_fail("file missing or corrupt", w, j);
        }
    }
    for (u64 w = 0; w < FS_STRESS_WORKERS; ++w) {
        for (u64 j = 0; j < FS_STRESS_FILES; ++j) {
            char name[24];
            fs_stress_name(name, w, j);
            if (!stellar::unlink(g_fs_stress_dir, name)) fs_stress_fail("unlink failed", w, j);
        }
    }
    if (!stellar::unlink(stellar::ROOT_STAR, "smpstress")) fs_stress_fail("could not remove the directory", 0, 0);
    u32 errors = g_fs_stress_errors;
    serial::printf("[smp-fs] %lu workers created %lu files in one directory, verified and removed them: %u error(s)\n",
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
    if (__atomic_sub_fetch(&g_fs_stress_remaining, 1u, __ATOMIC_SEQ_CST) == 0) fs_stress_finish();
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
    fb::init(info);
    fb::clear(0x0A0A14);
    fb::printf(0x8FD3FF, "Aphelion\n");
    fb::printf(0xB0B0C0, "non-POSIX x86_64 kernel -- framebuffer console online\n\n");

    serial::printf("[boot] framebuffer %lux%lu @ %u bpp\n", info.width, info.height, info.bpp);

    if (!hhdm_request.response || !rsdp_request.response) {
        serial::writeln("[boot] FATAL: missing HHDM or RSDP response");
        cpu::hang();
    }
    g_hhdm_offset = hhdm_request.response->offset;

    init_gdt_idt(true);
    fb::printf(0xC0FFC0, "[ok] GDT + TSS installed (per-core IST stacks for #DF/NMI)\n");
    fb::printf(0xC0FFC0, "[ok] IDT installed (256 vectors, no legacy PIC in the picture)\n");
    u64 early_apic_phys = apic::probe_mmio_phys_base();

    if (!memmap_request.response) {
        serial::writeln("[boot] FATAL: no memory map");
        cpu::hang();
    }
    auto* mm_resp = memmap_request.response;
    static universe::MemmapEntry entries[512];
    u64 n_entries = mm_resp->entry_count < 512 ? mm_resp->entry_count : 512;
    u64 max_phys = 0;
    for (u64 i = 0; i < n_entries; ++i) {
        entries[i] = { mm_resp->entries[i]->base, mm_resp->entries[i]->length, mm_resp->entries[i]->type };
        u64 top = entries[i].base + entries[i].length;

        bool huge_high_reserved = (entries[i].type == 1) && (entries[i].base >= 0x100000000ull);
        if (!huge_high_reserved && top > max_phys) max_phys = top;
    }
    universe::init(entries, n_entries, g_hhdm_offset);
    fb::printf(0xC0FFC0, "[ok] Universe online: %lu galaxies, %lu MiB free\n",
               static_cast<u64>(universe::galaxy_count()), universe::free_bytes() / (1024 * 1024));

    if (!kaddr_request.response) {
        serial::writeln("[boot] FATAL: no kernel address response");
        cpu::hang();
    }
    u64 kphys = kaddr_request.response->physical_base;
    u64 kvirt = kaddr_request.response->virtual_base;
    u64 ksize = static_cast<u64>(__kernel_end - __kernel_start);
    constellation::init(g_hhdm_offset, kphys, kvirt, ksize, max_phys);
    fb::printf(0xC0FFC0, "[ok] The Core constellation is live (own page tables, CR3 switched)\n");

    u64 lapic_2m = early_apic_phys & ~0x1FFFFFull;
    constellation::map_2m(g_hhdm_offset + lapic_2m, lapic_2m,
                           constellation::WRITABLE | constellation::NO_CACHE);

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

    acpi::init(reinterpret_cast<u64>(rsdp_request.response->address), g_hhdm_offset);
    fb::printf(0xC0FFC0, "[ok] ACPI/MADT parsed: %d CPU(s) reported\n",
               static_cast<int>(acpi::info().cpu_count));

    clock::init(apic::id());
    stellar::set_clock(&clock::now_ms);
    if (clock::wall_valid()) {
        char iso[21];
        civil::format_iso(civil::from_unix(clock::boot_wall_ms() / 1000), iso);
        fb::printf(0xC0FFC0, "[ok] RTC clock: %s, file mtimes are Unix milliseconds\n", iso);
    } else {
        fb::printf(0xE0D080, "[--] RTC unreadable; file mtimes stay 0\n");
    }

    constexpr u8 VEC_KEYBOARD = 0x21;
    if (acpi::info().ioapic_found) {
        u64 ioapic_2m = acpi::info().ioapic_base & ~0x1FFFFFull;
        constellation::map_2m(g_hhdm_offset + ioapic_2m, ioapic_2m,
                               constellation::WRITABLE | constellation::NO_CACHE);
        ioapic::init(g_hhdm_offset, acpi::info().ioapic_base, acpi::info().ioapic_gsi_base);
        keyboard::init(VEC_KEYBOARD);
        acpi::Redirection kbd_irq = acpi::resolve_isa_irq(1);
        ioapic::set_redirection(kbd_irq.gsi, VEC_KEYBOARD, apic::id(), false,
                                 kbd_irq.active_low, kbd_irq.level_triggered);
        fb::printf(0xC0FFC0, "[ok] IOAPIC online (%d redirection entries), PS/2 keyboard on IRQ1 -> GSI%u -- try typing\n",
                   static_cast<int>(ioapic::max_redirection_entries()), kbd_irq.gsi);
    } else {
        fb::printf(0xE0D080, "[--] No IOAPIC reported; keyboard unavailable\n");
    }

    virtioblk::init(g_hhdm_offset);
    ahci::init(g_hhdm_offset);
    nvme::init(g_hhdm_offset);
    if (blockdev::present()) {
        fb::printf(0xC0FFC0, "[ok] %s online (%lu sectors)\n",
                   blockdev::active()->name(), blockdev::capacity_sectors());

        stellar::init(g_hhdm_offset);
        stellar::Status mount_why = stellar::Status::Ok;
        bool fs_ready = stellar::mount(&mount_why);
        bool fresh_fs = false;
        if (!fs_ready) {
            if (mount_why == stellar::Status::NotFormatted && stellar::can_auto_format()) {
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
                bool o1_ok = (io_after.writes - io_before.writes == 1) && (io_after.reads - io_before.reads == 0) &&
                             snap_writes == 1 && snap_reads == 0;

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
            u64 expected_dir_count = STRESS_COUNT + 4;

            bool stress_ok = stress_create_ok && (stress_mismatches == 0) && (dir_count == expected_dir_count);
            if (stress_ok) {
                g_fs_stress_dir = stellar::find(stellar::ROOT_STAR, "smpstress");
                if (g_fs_stress_dir == stellar::INVALID_STAR)
                    g_fs_stress_dir = stellar::create_constellation(stellar::ROOT_STAR, "smpstress");
                g_fs_stress_enabled = g_fs_stress_dir != stellar::INVALID_STAR;
            }
            fb::printf(stress_ok ? 0xC0FFC0 : 0xE0D080,
                       "[%s] Stellar FS B+tree + growable directory stress: %lu stars, %lu mismatch(es), %lu dir entries enumerated\n",
                       stress_ok ? "ok" : "--", STRESS_COUNT, stress_mismatches, dir_count);
            serial::printf("[stellar] stress: %lu stars created (create_ok=%d), %lu mismatch(es), root dir enumerates %lu/%lu entries across its sector chain\n",
                            STRESS_COUNT, stress_create_ok ? 1 : 0, stress_mismatches, dir_count, expected_dir_count);
        } else {
            fb::printf(0xE0D080, "[--] Stellar FS: not available, see the serial log\n");
        }
    } else {
        fb::printf(0xB0B0C0, "[--] No block device found\n");
    }

    universe::reclaim_bootloader_regions(entries, n_entries);
    fb::printf(0xC0FFC0, "[ok] Bootloader-owned memory reclaimed: %lu MiB free\n",
               universe::free_bytes() / (1024 * 1024));

    orbital::init(g_hhdm_offset);

    g_cpus_online = 1;
    if (smp_request.response && smp_request.response->cpu_count > 1) {
        u64 total = smp_request.response->cpu_count;
        for (u64 i = 0; i < total; ++i) {
            limine_smp_info* c = smp_request.response->cpus[i];
            if (c->lapic_id == smp_request.response->bsp_lapic_id) continue;
            c->extra_argument = 0;
            c->goto_address = ap_entry;
        }
        fb::printf(0xC0FFC0, "[ok] SMP: waking %lu additional core(s)...\n", total - 1);

        for (u64 i = 0; i < 300000000ull && g_cpus_online < total; ++i) asm volatile("pause" ::: "memory");
        fb::printf(0xC0FFC0, "[ok] SMP: %lu Sun(s) online in total\n", g_cpus_online);
    } else {
        fb::printf(0xB0B0C0, "[--] SMP: single core reported, nothing else to wake\n");
    }

    fb::printf(0x8FD3FF, "\nOrbital scheduler: bringing up this core...\n");
    clock::sync();
    orbital::init_core();
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
    serial::writeln("[boot] === Aphelion is up. Handing off to the Orbital scheduler. ===");

    orbital::start_core();
}
