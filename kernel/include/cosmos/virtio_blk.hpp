#pragma once
#include <cosmos/types.hpp>

namespace virtioblk {

bool init(u64 hhdm_offset);
bool present();
u64 capacity_sectors();
bool using_msix();
u32 irq_count();

bool read_sector(u64 sector, void* buf512);
bool write_sector(u64 sector, const void* buf512);

}
