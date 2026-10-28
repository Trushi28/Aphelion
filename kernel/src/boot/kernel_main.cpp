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
#include <cosmos/boot_tests.hpp>

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

volatile u64 g_cpus_online = 0;
static volatile u64 g_next_core_idx = 1;
static u64 g_hhdm_offset = 0;

static constexpr u64 MAX_MEMMAP_ENTRIES = 512;
static universe::MemmapEntry g_entries[MAX_MEMMAP_ENTRIES];
static u64 g_n_entries = 0;
static u64 g_expected_cores = 1;

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

static char g_disk_note[112];

static void set_disk_note(const char* a, const char* b) {
    u32 n = 0;
    for (const char* p = a; p && *p && n + 1 < sizeof(g_disk_note); ++p) g_disk_note[n++] = *p;
    for (const char* p = b; p && *p && n + 1 < sizeof(g_disk_note); ++p) g_disk_note[n++] = *p;
    g_disk_note[n] = 0;
}

static NORETURN void panic_no_framebuffer() {
    serial::writeln("[boot] FATAL: bootloader gave no framebuffer");
    cpu::hang();
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

struct BootState {
    limine_framebuffer* lfb;
    bool console_started;
    u64 early_apic_phys;
    keyboard::Status kb_status;
    bool kb_irq;
    bool serial_irq;
    bool test_mode;
    bool usb_mode;
};

static void boot_framebuffer(BootState& s) {
    if (!fb_request.response || fb_request.response->framebuffer_count < 1)
        panic_no_framebuffer();

    limine_framebuffer* lfb = fb_request.response->framebuffers[0];
    s.lfb = lfb;
    fb::Info info{};
    info.base = static_cast<volatile u8*>(lfb->address);
    info.width = lfb->width; info.height = lfb->height; info.pitch = lfb->pitch;
    info.bpp = static_cast<u8>(lfb->bpp);
    info.red_shift = lfb->red_mask_shift;
    info.green_shift = lfb->green_mask_shift;
    info.blue_shift = lfb->blue_mask_shift;
    s.console_started = fb::init(info);
    if (!s.console_started)
        serial::writeln("[boot] the Corona console could not start (the framebuffer is not 32 bits per pixel with an aligned pitch); continuing on serial only");
    fb::clear(0x0A0A14);
    fb::printf(0x8FD3FF, "Aphelion\n");
    fb::printf(0xB0B0C0, "non-POSIX x86_64 kernel -- Corona console online\n\n");

    serial::printf("[boot] framebuffer %lux%lu @ %u bpp\n", info.width, info.height, info.bpp);
}

static void boot_config() {
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
}

static void boot_cpu(BootState& s) {
    percore::early_init();
    fpu::init_core();
    init_gdt_idt(true);
    fb::printf(0xC0FFC0, "[ok] GDT + TSS installed (per-core IST stacks for #DF/NMI)\n");
    fb::printf(0xC0FFC0, "[ok] IDT installed (256 vectors, no legacy PIC in the picture)\n");
    s.early_apic_phys = apic::probe_mmio_phys_base();
}

static void boot_memory(const BootState& s) {
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

    u64 fb_phys = (reinterpret_cast<u64>(s.lfb->address) - g_hhdm_offset) & ~0xFFFull;
    u64 fb_end = (reinterpret_cast<u64>(s.lfb->address) - g_hhdm_offset + s.lfb->pitch * s.lfb->height + 0xFFF) & ~0xFFFull;
    u64 fb_flags = constellation::WRITABLE | constellation::NO_EXECUTE;
    if (percore::wc_enabled()) fb_flags |= constellation::WRITE_COMBINING;
    if (!constellation::map_range(g_hhdm_offset + fb_phys, fb_phys, fb_end - fb_phys, fb_flags)) {
        serial::writeln("[boot] FATAL: could not map the framebuffer");
        cpu::hang();
    }
    fb::printf(0xC0FFC0, "[ok] The Core constellation is live (own page tables, CR3 switched)\n");

    map_device_page_or_die(s.early_apic_phys, "local APIC");
    bool ist_ok = gdt::guard_ist_stacks() && gdt::ist_guards_ok();
    serial::printf("[selftest] ist guards: %s\n", ist_ok ? "ok" : "MISMATCH");
    bool vmm_ok = constellation::selftest();
    serial::printf("[selftest] vmm: %s\n", vmm_ok ? "ok" : "MISMATCH");
}

static void boot_heap() {
    kheap::init();
    bool heap_ok = kheap::selftest();
    serial::printf("[selftest] kheap: %s\n", heap_ok ? "ok" : "MISMATCH");
    fb::printf(0xC0FFC0, "[ok] Kernel heap online: slab classes 16..%lu B, large blocks from Universe\n",
               static_cast<u64>(kheap::MAX_SMALL));
}

static void boot_console_buffer(const BootState& s) {
    if (s.console_started) {
        bool buffered = fb::enable_backbuffer();
        corona::Metrics cm = corona::metrics();
        serial::printf("[console] Corona %lux%lu cells of %lux%lu px on a %lux%lu framebuffer, %s\n",
                        static_cast<u64>(cm.cols), static_cast<u64>(cm.rows), static_cast<u64>(cm.cell_w), static_cast<u64>(cm.cell_h),
                        cm.fb_width, cm.fb_height,
                        buffered ? "RAM backbuffer on (the write-combined framebuffer is only ever written)" : "no backbuffer, drawing straight to the framebuffer");
        u32 history = fb::enable_scrollback();
        if (history) serial::printf("[console] scrollback holds %lu lines\n", static_cast<u64>(history));
        else serial::writeln("[console] scrollback unavailable, PageUp does nothing");
        bool console_ok = corona::selftest();
        serial::printf("[selftest] console: %s\n", console_ok ? "ok" : "MISMATCH");
    }
}

static void boot_platform() {
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
}

static void boot_input(BootState& s) {
    input::init();
    bool input_ok = input::selftest();
    serial::printf("[selftest] input: %s\n", input_ok ? "ok" : "MISMATCH");

    s.kb_status = keyboard::init(idt::VEC_KEYBOARD);
    serial::printf("[keyboard] i8042: %s, controller config %x\n", keyboard::status_name(s.kb_status),
                    static_cast<unsigned>(keyboard::config_byte()));
    s.kb_irq = false;
    s.serial_irq = false;
    if (acpi::info().ioapic_found) {
        map_device_page_or_die(acpi::info().ioapic_base, "IOAPIC");
        ioapic::init(g_hhdm_offset, acpi::info().ioapic_base, acpi::info().ioapic_gsi_base);
        if (s.kb_status == keyboard::Status::Ready) {
            acpi::Redirection kbd_irq = acpi::resolve_isa_irq(1);
            ioapic::set_redirection(kbd_irq.gsi, idt::VEC_KEYBOARD, apic::id(), false,
                                     kbd_irq.active_low, kbd_irq.level_triggered);
            keyboard::set_irq_routed(true);
            s.kb_irq = true;
        }
        if (serial::present() && serial::enable_rx_irq(idt::VEC_SERIAL)) {
            acpi::Redirection com1_irq = acpi::resolve_isa_irq(4);
            ioapic::set_redirection(com1_irq.gsi, idt::VEC_SERIAL, apic::id(), false,
                                     com1_irq.active_low, com1_irq.level_triggered);
            s.serial_irq = true;
        }
        fb::printf(0xC0FFC0, "[ok] IOAPIC online (%d redirection entries)\n", static_cast<int>(ioapic::max_redirection_entries()));
    } else {
        fb::printf(0xE0D080, "[--] No IOAPIC reported; keyboard and serial input fall back to polling\n");
    }
    if (s.kb_status == keyboard::Status::Ready)
        fb::printf(0xC0FFC0, "[ok] PS/2 keyboard %s\n", s.kb_irq ? "on IRQ1 -- try typing" : "polled (no IOAPIC)");
    else
        fb::printf(0xE0D080, "[--] No PS/2 keyboard controller: %s\n", keyboard::status_name(s.kb_status));
    serial::printf("[input] keyboard %s, serial %s\n", s.kb_irq ? "interrupt-driven" : (s.kb_status == keyboard::Status::Ready ? "polled" : "unavailable"),
                    serial::present() ? (s.serial_irq ? "interrupt-driven" : "polled") : "absent (no UART answered)");
}

static void boot_storage_drivers(const BootState& s) {
    if (bootcfg::get().storage == bootcfg::Storage::Auto) {
        virtioblk::init(g_hhdm_offset);
        ahci::init(g_hhdm_offset);
        nvme::init(g_hhdm_offset);
    } else if (s.usb_mode) {
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
    nebula::disk_set_confirmed_only(s.usb_mode);
}

static void boot_mount_for_shell(const BootState& s) {
    fb::printf(0xC0FFC0, "[ok] %s online (%lu sectors)\n", blockdev::active()->name(), blockdev::capacity_sectors());
    serial::printf("[storage] %s found; the filesystem test suite is skipped (%s)\n", blockdev::active()->name(),
                    s.usb_mode ? "the USB window is never formatted or written without an explicit command" : "shell mode");
    stellar::init(g_hhdm_offset);
    stellar::Status shell_mount_why = stellar::Status::Ok;
    bool shell_mounted = stellar::mount(&shell_mount_why);
    bool shell_formatted = false;
    if (!shell_mounted && !s.usb_mode && shell_mount_why == stellar::Status::NotFormatted && stellar::can_auto_format()) {
        serial::writeln("[stellar] blank disk: formatting");
        stellar::Status format_why = stellar::Status::Ok;
        shell_formatted = stellar::format(blockdev::capacity_sectors(), &format_why);
        if (shell_formatted) {
            shell_mounted = true;
        } else {
            shell_mount_why = format_why;
            serial::printf("[stellar] formatting the blank disk failed (%s)\n", stellar::status_name(format_why));
        }
    }
    if (shell_mounted) {
        nebula::disk_set_mounted(true);
        set_disk_note(s.usb_mode ? "USB window: Stellar volume mounted as disk:" : (shell_formatted ? "blank disk formatted, Stellar volume mounted as disk:" : "Stellar volume mounted as disk:"), nullptr);
        serial::printf("[nebula] Stellar volume %s for the shell: %lu sectors, %lu free\n", shell_formatted ? "created" : "mounted", stellar::total_sectors(), stellar::free_space_sectors());
    } else {
        const char* why_text = s.usb_mode && shell_mount_why == stellar::Status::NotFormatted ? "blank, not formatted yet" : stellar::status_name(shell_mount_why);
        set_disk_note(s.usb_mode ? "USB window has no Stellar volume: " : "no Stellar volume: ", why_text);
        serial::printf("[nebula] no Stellar volume for the shell (%s); %s\n", why_text,
                        s.usb_mode ? "storage init erase <MiB> creates one in the USB window" : "mkfs erase creates one on a blank disk");
    }
    if (s.usb_mode) {
        serial::printf("[storage] %s\n", g_disk_note);
        fb::printf(nebula::disk_is_mounted() ? 0xC0FFC0 : 0xE0D080, "[%s] %s\n", nebula::disk_is_mounted() ? "ok" : "--", g_disk_note);
    }
}

static void boot_test_volume() {
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
            u64 fs_sectors = capacity > boottest::TEST_TAIL_SECTORS + 64 ? capacity - boottest::TEST_TAIL_SECTORS : capacity;
            fs_ready = stellar::format(fs_sectors);
            fresh_fs = fs_ready;
        } else {
            serial::printf("[stellar] refusing to format: mount failed (%s)%s\n", stellar::status_name(mount_why),
                            mount_why == stellar::Status::NotFormatted ? ", and the disk holds data that is not a Stellar volume" : "");
            fb::printf(0xE0D080, "[--] Stellar FS: mount failed (%s), disk left untouched\n", stellar::status_name(mount_why));
        }
    }
    bool tail_free = fs_ready && stellar::total_sectors() + boottest::TEST_TAIL_SECTORS <= blockdev::capacity_sectors();
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
    if (fs_ready) boottest::run_fs_selftests(fresh_fs);
    else fb::printf(0xE0D080, "[--] Stellar FS: not available, see the serial log\n");
}

static void boot_orbital_init() {
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
}

static void boot_wake_cores() {
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
}

static void boot_core_selftests(const BootState& s) {
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

    if (s.test_mode) {
        bool nebula_ok = nebula::selftest();
        serial::printf("[selftest] nebula: %s\n", nebula_ok ? "ok" : "MISMATCH");
    }
}

static void boot_start_scheduler(const BootState& s) {
    fb::printf(0x8FD3FF, "\nOrbital scheduler: bringing up this core...\n");
    clock::sync();
    orbital::init_core();
    corona::enable_locking();
    if (s.test_mode) boottest::spawn_test_tasks();
}

static void boot_start_shell(const BootState& s) {
    helm::Facts facts{};
    facts.suns = static_cast<u32>(g_cpus_online);
    facts.storage = blockdev::present() ? blockdev::active()->name()
                    : (bootcfg::get().storage == bootcfg::Storage::Off ? "disabled by the boot configuration" : "no block device found");
    facts.usb_mode = s.usb_mode;
    facts.keyboard_ready = s.kb_status == keyboard::Status::Ready;
    facts.keyboard_irq = s.kb_irq;
    facts.serial_present = serial::present();
    facts.serial_irq = s.serial_irq;
    facts.disk_mounted = nebula::disk_is_mounted();
    facts.disk_note = g_disk_note;
    helm::set_facts(facts);
    nebula::sys_set_facts(facts.storage, facts.suns);
    if (!helm::spawn()) serial::writeln("[boot] FATAL: could not start the Helm shell");
}

extern "C" NORETURN void kernel_main() {
    serial::init();
    serial::writeln("\n=== Aphelion booting ===");

    if (!LIMINE_BASE_REVISION_SUPPORTED) {
        serial::writeln("[boot] FATAL: Limine base revision unsupported");
        cpu::hang();
    }

    BootState s{};
    boot_framebuffer(s);
    boot_config();
    s.test_mode = bootcfg::get().mode == bootcfg::Mode::Test;
    s.usb_mode = bootcfg::get().storage == bootcfg::Storage::Usb;
    boot_cpu(s);
    boot_memory(s);
    boot_heap();
    boot_console_buffer(s);
    boot_platform();
    boot_input(s);
    boot_storage_drivers(s);
    if (blockdev::present() && (!s.test_mode || s.usb_mode)) boot_mount_for_shell(s);
    else if (blockdev::present()) boot_test_volume();
    else fb::printf(0xB0B0C0, "[--] No block device found\n");
    boot_orbital_init();
    boot_wake_cores();
    boot_core_selftests(s);
    boot_start_scheduler(s);
    serial::writeln("[boot] === Aphelion is up. Handing off to the Orbital scheduler. ===");
    if (s.test_mode) boottest::spawn_mm_stress();
    else boot_start_shell(s);
    orbital::spawn("reclaim", &reclaim_task, nullptr);
    if (s.test_mode) orbital::spawn_selftest();
    orbital::start_core();
}
