#pragma once
#include <cosmos/types.hpp>

namespace ioapic {

void init(u64 hhdm_offset, u64 ioapic_phys, u32 gsi_base);

u32 max_redirection_entries();

void set_redirection(u32 gsi, u8 vector, u32 dest_apic_id, bool masked,
                      bool active_low, bool level_triggered);

}
