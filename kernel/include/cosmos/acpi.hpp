#pragma once
#include <cosmos/types.hpp>

namespace acpi {

constexpr u32 MAX_CPUS = 64;
constexpr u32 MAX_ISO = 16;

struct IsoEntry {
    u8 source_irq = 0;
    u32 gsi = 0;
    u16 flags = 0;
};

struct Info {
    u32 lapic_ids[MAX_CPUS];
    u32 cpu_count = 0;
    u64 ioapic_base = 0;
    u32 ioapic_gsi_base = 0;
    bool ioapic_found = false;
    IsoEntry iso[MAX_ISO];
    u32 iso_count = 0;
    bool hpet_found = false;
    u64 hpet_base = 0;
    bool pm_timer_found = false;
    u16 pm_timer_port = 0;
    bool pm_timer_32bit = false;
    bool fadt_found = false;
    u8 fadt_revision = 0;
    bool iapc_known = false;
    u16 iapc_boot_arch = 0;
    bool reset_supported = false;
    u8 reset_space = 0;
    u64 reset_address = 0;
    u8 reset_value = 0;
};

constexpr u16 IAPC_LEGACY_DEVICES = 1u << 0;
constexpr u16 IAPC_8042 = 1u << 1;
constexpr u16 IAPC_NO_VGA = 1u << 2;
constexpr u16 IAPC_NO_MSI = 1u << 3;
constexpr u16 IAPC_NO_ASPM = 1u << 4;
constexpr u16 IAPC_NO_CMOS_RTC = 1u << 5;

struct Redirection {
    u32 gsi;
    bool active_low;
    bool level_triggered;
};

void init(u64 rsdp_phys);
const Info& info();
Redirection resolve_isa_irq(u8 isa_irq);

}
