#pragma once
#include <cosmos/types.hpp>

namespace apic {

bool x2apic_supported();
bool using_x2apic();

u64 probe_mmio_phys_base();

void init(u64 hhdm_offset);
void eoi();
u32  id();
void start_timer_periodic(u32 vector, u32 initial_count, u8 divide_pow2);

void start_timer_stopwatch(u8 divide_pow2);
u32  read_timer_current();

void send_init_ipi(u32 target_apic_id);
void send_startup_ipi(u32 target_apic_id, u8 vector  );

u64 xapic_mmio_phys_base();

}
