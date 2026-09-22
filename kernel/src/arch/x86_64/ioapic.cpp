#include <cosmos/ioapic.hpp>

namespace ioapic {

constexpr u32 REG_IOREGSEL = 0x00;
constexpr u32 REG_IOWIN    = 0x10;
constexpr u32 REDTBL_BASE  = 0x10;

static volatile u32* g_mmio = nullptr;

static u32 read_reg(u32 index) {
    g_mmio[REG_IOREGSEL / 4] = index;
    return g_mmio[REG_IOWIN / 4];
}
static void write_reg(u32 index, u32 value) {
    g_mmio[REG_IOREGSEL / 4] = index;
    g_mmio[REG_IOWIN / 4] = value;
}

void init(u64 hhdm_offset, u64 ioapic_phys, u32  ) {

    g_mmio = reinterpret_cast<volatile u32*>(hhdm_offset + ioapic_phys);
}

u32 max_redirection_entries() {

    return ((read_reg(0x01) >> 16) & 0xFF) + 1;
}

void set_redirection(u32 gsi, u8 vector, u32 dest_apic_id, bool masked,
                      bool active_low, bool level_triggered) {
    u32 low = vector;
    if (masked) low |= (1u << 16);
    if (active_low) low |= (1u << 13);
    if (level_triggered) low |= (1u << 15);
    u32 high = (dest_apic_id & 0xFFu) << 24;

    write_reg(REDTBL_BASE + gsi * 2 + 1, high);
    write_reg(REDTBL_BASE + gsi * 2, low);
}

}
