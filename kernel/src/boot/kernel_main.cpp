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

constexpr u64 WORKER_COUNT = 8;
static volatile u64 g_worker_cores_mask = 0;
static volatile u32 g_workers_remaining = WORKER_COUNT;

static void demo_worker(void* arg) {
    u64 idx = reinterpret_cast<u64>(arg);
    serial::printf("[demo] worker %lu started on core %d\n", idx, static_cast<int>(apic::id()));
    for (u32 i = 0; i < 50000; ++i) {
        if ((i & 0x7FF) == 0) orbital::yield();
    }
    u32 core = apic::id();
    serial::printf("[demo] worker %lu finished on core %d\n", idx, static_cast<int>(core));
    u32 bit = core < 64 ? core : 63;
    __atomic_fetch_or(&g_worker_cores_mask, 1ull << bit, __ATOMIC_SEQ_CST);
    if (__atomic_sub_fetch(&g_workers_remaining, 1u, __ATOMIC_SEQ_CST) == 0) {
        u64 mask = g_worker_cores_mask;
        u32 distinct = 0;
        while (mask) { distinct += static_cast<u32>(mask & 1); mask >>= 1; }
        bool ok = distinct > 1;
        serial::printf("[orbital] work-stealing check: %lu worker(s) finished across %u distinct core(s) -> %s\n",
                        WORKER_COUNT, distinct, ok ? "stealing confirmed" : "no migration observed");
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

        u64 total_sectors = blockdev::capacity_sectors();
        if (total_sectors > 64) {
            constexpr u64 SCRATCH_COUNT = 32;
            u64 scratch_start = total_sectors - SCRATCH_COUNT;
            static u8 scratch_write[SCRATCH_COUNT * 512];
            static u8 scratch_read[SCRATCH_COUNT * 512];
            for (u64 i = 0; i < sizeof(scratch_write); ++i)
                scratch_write[i] = static_cast<u8>((i * 2654435761u) >> 24);
            bool batch_write_ok = blockdev::write_sectors(scratch_start, SCRATCH_COUNT, scratch_write);
            bool batch_read_ok = blockdev::read_sectors(scratch_start, SCRATCH_COUNT, scratch_read);
            bool batch_match = batch_write_ok && batch_read_ok;
            for (u64 i = 0; batch_match && i < sizeof(scratch_write); ++i)
                batch_match = (scratch_write[i] == scratch_read[i]);
            fb::printf(batch_match ? 0xC0FFC0 : 0xE0D080,
                       "[%s] %s multi-request queueing: %lu sectors written+read in one batch, %s\n",
                       batch_match ? "ok" : "--", blockdev::active()->name(), SCRATCH_COUNT,
                       batch_match ? "byte-for-byte match" : "MISMATCH");
        }

        stellar::init(g_hhdm_offset);
        bool fs_ready = stellar::mount();
        if (!fs_ready) fs_ready = stellar::format(blockdev::capacity_sectors());

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
            nested_readback[nested_n] = 0;
            serial::printf("[stellar] /sub/nested.txt (%lu bytes): %s\n",
                            nested_n, reinterpret_cast<const char*>(nested_readback));

            static u8 readback[128];
            u64 n = stellar::read_file(file, readback, sizeof(readback) - 1);
            readback[n] = 0;

            bool match = (n == msg_len);
            for (u64 i = 0; match && i < n; ++i) match = (readback[i] == static_cast<u8>(msg[i]));

            fb::printf(match ? 0xC0FFC0 : 0xE0D080,
                       "[%s] Stellar FS: created /hello.txt, read %lu bytes back, %s\n",
                       match ? "ok" : "--", n, match ? "matched exactly" : "MISMATCH");
            serial::printf("[stellar] readback: %s\n", reinterpret_cast<const char*>(readback));
            if (virtioblk::present())
                serial::printf("[virtio-blk] completion mode: %s, %u interrupt(s) delivered\n",
                                virtioblk::using_msix() ? "MSI-X" : "polled", virtioblk::irq_count());
            if (ahci::present())
                serial::printf("[ahci] completion mode: %s, %u interrupt(s) delivered\n",
                                ahci::using_msi() ? "MSI" : "polled", ahci::irq_count());
            if (nvme::present())
                serial::printf("[nvme] completion mode: %s, %u interrupt(s) delivered\n",
                                nvme::completion_mode(), nvme::irq_count());

            const char* rewritten_msg = "Aphelion Stellar FS -- rewritten after snapshot.\n";
            u64 rewritten_len = 0;
            while (rewritten_msg[rewritten_len]) ++rewritten_len;

            bool cow_pre_ok = stellar::verify_file(file);
            u64 snap_id = stellar::snapshot(file);
            u64 rewritten_star = stellar::write_file(file, rewritten_msg, rewritten_len);
            bool cow_write_ok = (rewritten_star == file) && stellar::verify_file(file);
            bool cow_snap_ok = (snap_id != stellar::INVALID_STAR) && stellar::verify_file(snap_id);

            static u8 snap_readback[128];
            u64 snap_n = stellar::read_file(snap_id, snap_readback, sizeof(snap_readback) - 1);
            snap_readback[snap_n] = 0;
            bool snap_content_ok = (snap_n == msg_len);
            for (u64 i = 0; snap_content_ok && i < snap_n; ++i)
                snap_content_ok = (snap_readback[i] == static_cast<u8>(msg[i]));

            static u8 new_readback[128];
            u64 new_n = stellar::read_file(file, new_readback, sizeof(new_readback) - 1);
            new_readback[new_n] = 0;
            bool new_content_ok = (new_n == rewritten_len);
            for (u64 i = 0; new_content_ok && i < new_n; ++i)
                new_content_ok = (new_readback[i] == static_cast<u8>(rewritten_msg[i]));

            bool cow_ok = cow_pre_ok && cow_write_ok && cow_snap_ok && snap_content_ok && new_content_ok;
            fb::printf(cow_ok ? 0xC0FFC0 : 0xE0D080,
                       "[%s] Stellar FS COW: snapshot star %lu keeps the pre-write bytes, live star %lu carries the rewrite, both checksums verify\n",
                       cow_ok ? "ok" : "--", snap_id, file);
            serial::printf("[stellar] cow: snapshot readback \"%s\" (%lu bytes), live readback \"%s\" (%lu bytes)\n",
                            reinterpret_cast<const char*>(snap_readback), snap_n,
                            reinterpret_cast<const char*>(new_readback), new_n);

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

            constexpr u64 STRESS_COUNT = 14;
            static u64 stress_ids[STRESS_COUNT];
            bool stress_create_ok = true;
            for (u64 i = 0; i < STRESS_COUNT; ++i) {
                char digits[20];
                u32 dlen = itoa_dec(i, digits);
                char name[32] = "stress";
                for (u32 j = 0; j < dlen; ++j) name[6 + j] = digits[j];
                name[6 + dlen] = 0;
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
            u64 expected_dir_count = STRESS_COUNT + 2;

            bool stress_ok = stress_create_ok && (stress_mismatches == 0) && (dir_count >= expected_dir_count);
            fb::printf(stress_ok ? 0xC0FFC0 : 0xE0D080,
                       "[%s] Stellar FS B+tree + growable directory stress: %lu stars, %lu mismatch(es), %lu dir entries enumerated\n",
                       stress_ok ? "ok" : "--", STRESS_COUNT, stress_mismatches, dir_count);
            serial::printf("[stellar] stress: %lu stars created (create_ok=%d), %lu mismatch(es), root dir enumerates %lu/%lu entries across its sector chain\n",
                            STRESS_COUNT, stress_create_ok ? 1 : 0, stress_mismatches, dir_count, expected_dir_count);
        } else {
            fb::printf(0xE0D080, "[--] Stellar FS: mount and format both failed\n");
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
    orbital::init_core();
    orbital::spawn("alpha", &demo_cooperative, const_cast<char*>("alpha"));
    orbital::spawn("beta", &demo_cooperative_explicit_exit, const_cast<char*>("beta"));
    orbital::spawn("gamma-spinner", &demo_spinner, const_cast<char*>("gamma-spinner"));
    for (u64 i = 0; i < WORKER_COUNT; ++i)
        orbital::spawn("worker", &demo_worker, reinterpret_cast<void*>(i));
    fb::printf(0xC0FFC0, "[ok] Orbital scheduler online -- 3 demo Satellites + %lu work-stealing workers spawned\n",
               WORKER_COUNT);
    serial::writeln("[boot] === Aphelion is up. Handing off to the Orbital scheduler. ===");

    orbital::start_core();
}
