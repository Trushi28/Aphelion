#include <cosmos/pci.hpp>
#include <cosmos/cpu.hpp>

namespace pci {

constexpr u16 PORT_ADDR = 0xCF8;
constexpr u16 PORT_DATA = 0xCFC;

static u32 make_address(Address a, u8 offset) {
    return (1u << 31) | (static_cast<u32>(a.bus) << 16) | (static_cast<u32>(a.device) << 11) |
           (static_cast<u32>(a.function) << 8) | (offset & 0xFC);
}

u32 read32(Address addr, u8 offset) {
    cpu::out32(PORT_ADDR, make_address(addr, offset));
    return cpu::in32(PORT_DATA);
}
u16 read16(Address addr, u8 offset) {
    u32 v = read32(addr, offset & 0xFC);
    return static_cast<u16>(v >> ((offset & 2) * 8));
}
u8 read8(Address addr, u8 offset) {
    u32 v = read32(addr, offset & 0xFC);
    return static_cast<u8>(v >> ((offset & 3) * 8));
}
void write32(Address addr, u8 offset, u32 value) {
    cpu::out32(PORT_ADDR, make_address(addr, offset));
    cpu::out32(PORT_DATA, value);
}
void write16(Address addr, u8 offset, u16 value) {
    u32 old = read32(addr, offset & 0xFC);
    u32 shift = (offset & 2) * 8;
    u32 mask = 0xFFFFu << shift;
    u32 nv = (old & ~mask) | (static_cast<u32>(value) << shift);
    write32(addr, offset & 0xFC, nv);
}

void scan(ScanCallback cb, void* ctx) {
    for (u32 bus = 0; bus < 256; ++bus) {
        for (u32 dev = 0; dev < 32; ++dev) {
            Address a0{static_cast<u8>(bus), static_cast<u8>(dev), 0};
            u16 vendor0 = read16(a0, 0x00);
            if (vendor0 == 0xFFFF) continue;

            u8 header_type = read8(a0, 0x0E);
            u32 max_func = (header_type & 0x80) ? 8 : 1;

            for (u32 fn = 0; fn < max_func; ++fn) {
                Address a{static_cast<u8>(bus), static_cast<u8>(dev), static_cast<u8>(fn)};
                u16 vendor = read16(a, 0x00);
                if (vendor == 0xFFFF) continue;

                Device d;
                d.addr = a;
                d.vendor_id = vendor;
                d.device_id = read16(a, 0x02);
                u32 classreg = read32(a, 0x08);
                d.class_code = static_cast<u8>(classreg >> 24);
                d.subclass = static_cast<u8>(classreg >> 16);
                d.prog_if = static_cast<u8>(classreg >> 8);
                cb(d, ctx);
            }
        }
    }
}

u64 bar_address(Address addr, int bar_index) {
    u8 off = static_cast<u8>(0x10 + 4 * bar_index);
    u32 low = read32(addr, off);
    if (low & 1) return low & ~0x3u;
    u32 type = (low >> 1) & 0x3;
    u64 base = low & ~0xFu;
    if (type == 2) {
        u32 high = read32(addr, static_cast<u8>(off + 4));
        base |= static_cast<u64>(high) << 32;
    }
    return base;
}

}
