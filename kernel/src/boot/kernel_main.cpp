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
        ioapic::init(g_hhdm_offset, acpi::info().ioapic_base, acpi::info().ioapic_gsi_base);
        keyboard::init(VEC_KEYBOARD);
        ioapic::set_redirection(1, VEC_KEYBOARD, apic::id(),   false);
        fb::printf(0xC0FFC0, "[ok] IOAPIC online (%d redirection entries), PS/2 keyboard on IRQ1 -- try typing\n",
                   static_cast<int>(ioapic::max_redirection_entries()));
    } else {
        fb::printf(0xE0D080, "[--] No IOAPIC reported; keyboard unavailable\n");
    }

    if (virtioblk::init(g_hhdm_offset)) {
        fb::printf(0xC0FFC0, "[ok] virtio-blk online (%lu sectors)\n", virtioblk::capacity_sectors());

        stellar::init(g_hhdm_offset);
        bool fs_ready = stellar::mount();
        if (!fs_ready) fs_ready = stellar::format(virtioblk::capacity_sectors());

        if (fs_ready) {
            const char* msg = "Aphelion Stellar FS -- real file, real disk, real bytes.\n";
            u64 msg_len = 0;
            while (msg[msg_len]) ++msg_len;

            u32 file = stellar::find(stellar::ROOT_STAR, "hello.txt");
            if (file == stellar::INVALID_STAR)
                file = stellar::create_file(stellar::ROOT_STAR, "hello.txt", msg, msg_len);

            u32 subdir = stellar::find(stellar::ROOT_STAR, "sub");
            if (subdir == stellar::INVALID_STAR)
                subdir = stellar::create_constellation(stellar::ROOT_STAR, "sub");
            const char* nested_msg = "nested constellation works\n";
            u64 nested_len = 0;
            while (nested_msg[nested_len]) ++nested_len;
            u32 nested_file = stellar::find(subdir, "nested.txt");
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
        } else {
            fb::printf(0xE0D080, "[--] Stellar FS: mount and format both failed\n");
        }
    } else {
        fb::printf(0xB0B0C0, "[--] No virtio-blk device on the PCI bus\n");
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

        for (u64 i = 0; i < 200000000ull; ++i) { asm volatile("" ::: "memory"); }
        fb::printf(0xC0FFC0, "[ok] SMP: %lu Sun(s) online in total\n", g_cpus_online);
    } else {
        fb::printf(0xB0B0C0, "[--] SMP: single core reported, nothing else to wake\n");
    }

    fb::printf(0x8FD3FF, "\nOrbital scheduler: bringing up this core...\n");
    orbital::init_core();
    orbital::spawn("alpha", &demo_cooperative, const_cast<char*>("alpha"));
    orbital::spawn("beta", &demo_cooperative_explicit_exit, const_cast<char*>("beta"));
    orbital::spawn("gamma-spinner", &demo_spinner, const_cast<char*>("gamma-spinner"));
    fb::printf(0xC0FFC0, "[ok] Orbital scheduler online -- 3 demo Satellites spawned\n");
    serial::writeln("[boot] === Aphelion is up. Handing off to the Orbital scheduler. ===");

    orbital::start_core();
}
