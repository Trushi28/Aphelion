#pragma once
#include <cosmos/types.hpp>

namespace acpi {

constexpr u32 MAX_CPUS = 64;

struct Info {
    u32 lapic_ids[MAX_CPUS];
    u32 cpu_count = 0;
    u64 ioapic_base = 0;
    u32 ioapic_gsi_base = 0;
    bool ioapic_found = false;
};

void init(u64 rsdp_phys, u64 hhdm_offset);
const Info& info();

}
