#pragma once
#include <cosmos/types.hpp>

namespace blockdev {

struct Device {
    virtual bool read_sector(u64 sector, void* buf512) = 0;
    virtual bool write_sector(u64 sector, const void* buf512) = 0;
    virtual u64 capacity_sectors() = 0;
    virtual const char* name() = 0;
};

void register_device(Device* dev);
bool present();
Device* active();

bool read_sector(u64 sector, void* buf512);
bool write_sector(u64 sector, const void* buf512);
u64 capacity_sectors();

}
