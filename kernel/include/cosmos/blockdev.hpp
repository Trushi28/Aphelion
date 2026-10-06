#pragma once
#include <cosmos/types.hpp>

namespace blockdev {

struct Device {
    virtual bool read_sector(u64 sector, void* buf512) = 0;
    virtual bool write_sector(u64 sector, const void* buf512) = 0;
    virtual bool read_sectors(u64 start_sector, u64 count, void* buf) {
        u8* p = static_cast<u8*>(buf);
        for (u64 i = 0; i < count; ++i)
            if (!read_sector(start_sector + i, p + i * 512)) return false;
        return true;
    }
    virtual bool write_sectors(u64 start_sector, u64 count, const void* buf) {
        const u8* p = static_cast<const u8*>(buf);
        for (u64 i = 0; i < count; ++i)
            if (!write_sector(start_sector + i, p + i * 512)) return false;
        return true;
    }
    virtual bool flush() { return true; }
    virtual u64 capacity_sectors() = 0;
    virtual const char* name() = 0;
};

void register_device(Device* dev);
bool present();
Device* active();

bool read_sector(u64 sector, void* buf512);
bool write_sector(u64 sector, const void* buf512);
bool read_sectors(u64 start_sector, u64 count, void* buf);
bool write_sectors(u64 start_sector, u64 count, const void* buf);
u64 capacity_sectors();
bool flush();

}
