#pragma once
#include <cosmos/types.hpp>

namespace pci {

struct Address {
    u8 bus, device, function;
};

u32 read32(Address addr, u8 offset);
u16 read16(Address addr, u8 offset);
u8  read8(Address addr, u8 offset);
void write32(Address addr, u8 offset, u32 value);
void write16(Address addr, u8 offset, u16 value);

struct Device {
    Address addr;
    u16 vendor_id, device_id;
    u8 class_code, subclass, prog_if;
};

using ScanCallback = void (*)(const Device&, void* ctx);
void scan(ScanCallback cb, void* ctx);

u64 bar_address(Address addr, int bar_index);

}
