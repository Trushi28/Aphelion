#include <cosmos/blockdev.hpp>
#include <cosmos/serial.hpp>

namespace blockdev {

static Device* g_active = nullptr;

void register_device(Device* dev) {
    if (g_active) return;
    g_active = dev;
    serial::printf("[blockdev] %s registered as the active block device\n", dev->name());
}

bool present() { return g_active != nullptr; }
Device* active() { return g_active; }

bool read_sector(u64 sector, void* buf512) {
    return g_active && g_active->read_sector(sector, buf512);
}
bool write_sector(u64 sector, const void* buf512) {
    return g_active && g_active->write_sector(sector, buf512);
}
u64 capacity_sectors() {
    return g_active ? g_active->capacity_sectors() : 0;
}

}
