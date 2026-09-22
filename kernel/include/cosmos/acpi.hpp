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
};

struct Redirection {
    u32 gsi;
    bool active_low;
    bool level_triggered;
};

void init(u64 rsdp_phys, u64 hhdm_offset);
const Info& info();
Redirection resolve_isa_irq(u8 isa_irq);

}
