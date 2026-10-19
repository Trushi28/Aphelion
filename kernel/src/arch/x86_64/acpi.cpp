#include <cosmos/acpi.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/vmm.hpp>

namespace acpi {

struct PACKED Rsdp {
    char sig[8];
    u8 checksum;
    char oem_id[6];
    u8 revision;
    u32 rsdt_addr;

    u32 length;
    u64 xsdt_addr;
    u8 ext_checksum;
    u8 reserved[3];
};

struct PACKED SdtHeader {
    char sig[4];
    u32 length;
    u8 revision;
    u8 checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32 oem_revision;
    u32 creator_id;
    u32 creator_revision;
};

struct PACKED MadtEntry {
    u8 type;
    u8 length;
};

struct PACKED MadtLocalApic {
    MadtEntry hdr;
    u8 acpi_processor_id;
    u8 apic_id;
    u32 flags;
};

struct PACKED MadtIoApic {
    MadtEntry hdr;
    u8 ioapic_id;
    u8 reserved;
    u32 ioapic_addr;
    u32 gsi_base;
};

struct PACKED MadtLocalX2Apic {
    MadtEntry hdr;
    u16 reserved;
    u32 x2apic_id;
    u32 flags;
    u32 acpi_processor_uid;
};

struct PACKED MadtIso {
    MadtEntry hdr;
    u8 bus;
    u8 source_irq;
    u32 gsi;
    u16 flags;
};

struct PACKED GenericAddress {
    u8 space_id;
    u8 bit_width;
    u8 bit_offset;
    u8 access_size;
    u64 address;
};

struct PACKED HpetTable {
    SdtHeader hdr;
    u32 event_timer_block_id;
    GenericAddress base;
    u8 hpet_number;
    u16 minimum_tick;
    u8 page_protection;
};

constexpr u32 FADT_PM_TMR_BLK = 76;
constexpr u32 FADT_IAPC_BOOT_ARCH = 109;
constexpr u32 FADT_FLAGS = 112;
constexpr u32 FADT_RESET_REG = 116;
constexpr u32 FADT_RESET_VALUE = 128;
constexpr u32 FADT_FLAG_RESET_REG_SUP = 1u << 10;
constexpr u32 FADT_X_PM_TMR_BLK = 208;
constexpr u32 FADT_FLAG_TMR_VAL_EXT = 1u << 8;
constexpr u8 SPACE_SYSTEM_MEMORY = 0;
constexpr u8 SPACE_SYSTEM_IO = 1;

static Info g_info;
constexpr u32 MAX_TABLE_BYTES = 16u << 20;

template <typename T> static T* map(u64 addr, u64 bytes) {
    return reinterpret_cast<T*>(constellation::map_firmware(addr, bytes));
}

static SdtHeader* map_sdt(u64 addr) {
    auto* head = map<SdtHeader>(addr, sizeof(SdtHeader));
    if (!head) return nullptr;
    u32 length = head->length;
    if (length < sizeof(SdtHeader) || length > MAX_TABLE_BYTES) return nullptr;
    return map<SdtHeader>(addr, length);
}

static bool sig_matches(const char* a, const char* b, int n) {
    for (int i = 0; i < n; ++i) if (a[i] != b[i]) return false;
    return true;
}

static void parse_madt(SdtHeader* madt) {
    auto* base = reinterpret_cast<u8*>(madt);
    u8* p = base + sizeof(SdtHeader) + 8;
    u8* end = base + madt->length;

    while (p < end) {
        auto* e = reinterpret_cast<MadtEntry*>(p);
        if (e->length == 0) break;
        switch (e->type) {
            case 0: {
                auto* la = reinterpret_cast<MadtLocalApic*>(p);
                if ((la->flags & 1) && g_info.cpu_count < MAX_CPUS)
                    g_info.lapic_ids[g_info.cpu_count++] = la->apic_id;
                break;
            }
            case 9: {
                auto* xa = reinterpret_cast<MadtLocalX2Apic*>(p);
                if ((xa->flags & 1) && g_info.cpu_count < MAX_CPUS)
                    g_info.lapic_ids[g_info.cpu_count++] = xa->x2apic_id;
                break;
            }
            case 1: {
                auto* io = reinterpret_cast<MadtIoApic*>(p);
                g_info.ioapic_base = io->ioapic_addr;
                g_info.ioapic_gsi_base = io->gsi_base;
                g_info.ioapic_found = true;
                break;
            }
            case 2: {
                auto* iso = reinterpret_cast<MadtIso*>(p);
                if (g_info.iso_count < MAX_ISO) {
                    g_info.iso[g_info.iso_count].source_irq = iso->source_irq;
                    g_info.iso[g_info.iso_count].gsi = iso->gsi;
                    g_info.iso[g_info.iso_count].flags = iso->flags;
                    ++g_info.iso_count;
                }
                break;
            }
            default: break;
        }
        p += e->length;
    }
}

static void parse_hpet(SdtHeader* t) {
    if (t->length < sizeof(HpetTable)) return;
    auto* h = reinterpret_cast<HpetTable*>(t);
    if (h->base.space_id != SPACE_SYSTEM_MEMORY || h->base.address == 0) return;
    g_info.hpet_found = true;
    g_info.hpet_base = h->base.address;
}

template <typename T> static T field(const SdtHeader* t, u32 offset) {
    T v;
    __builtin_memcpy(&v, reinterpret_cast<const u8*>(t) + offset, sizeof(T));
    return v;
}

static void parse_fadt(SdtHeader* t) {
    g_info.fadt_found = true;
    g_info.fadt_revision = t->revision;
    u32 flags = t->length >= FADT_FLAGS + 4 ? field<u32>(t, FADT_FLAGS) : 0;

    if (t->revision >= 2 && t->length >= FADT_IAPC_BOOT_ARCH + 2) {
        g_info.iapc_known = true;
        g_info.iapc_boot_arch = field<u16>(t, FADT_IAPC_BOOT_ARCH);
    }

    if ((flags & FADT_FLAG_RESET_REG_SUP) && t->length >= FADT_RESET_VALUE + 1) {
        auto gas = field<GenericAddress>(t, FADT_RESET_REG);
        bool usable = (gas.space_id == SPACE_SYSTEM_IO && gas.address <= 0xFFFF) ||
                      (gas.space_id == SPACE_SYSTEM_MEMORY && gas.address != 0);
        if (usable) {
            g_info.reset_supported = true;
            g_info.reset_space = gas.space_id;
            g_info.reset_address = gas.address;
            g_info.reset_value = field<u8>(t, FADT_RESET_VALUE);
        }
    }

    u64 port = 0;
    if (t->length >= FADT_PM_TMR_BLK + 4) port = field<u32>(t, FADT_PM_TMR_BLK);
    if (t->length >= FADT_X_PM_TMR_BLK + sizeof(GenericAddress)) {
        auto gas = field<GenericAddress>(t, FADT_X_PM_TMR_BLK);
        if (gas.space_id == SPACE_SYSTEM_IO && gas.address != 0) port = gas.address;
    }
    if (port == 0 || port > 0xFFFC) return;
    g_info.pm_timer_found = true;
    g_info.pm_timer_port = static_cast<u16>(port);
    g_info.pm_timer_32bit = (flags & FADT_FLAG_TMR_VAL_EXT) != 0;
}

void init(u64 rsdp_phys) {
    g_info = Info{};

    auto* rsdp = map<Rsdp>(rsdp_phys, sizeof(Rsdp));
    if (!rsdp) {
        serial::writeln("[acpi] could not map the RSDP");
        return;
    }
    bool use_xsdt = rsdp->revision >= 2 && rsdp->xsdt_addr != 0;

    SdtHeader* root = map_sdt(use_xsdt ? rsdp->xsdt_addr : rsdp->rsdt_addr);
    if (!root) {
        serial::writeln("[acpi] could not map the root table");
        return;
    }

    u32 entry_count = (root->length - sizeof(SdtHeader)) / (use_xsdt ? 8 : 4);
    u8* entries = reinterpret_cast<u8*>(root) + sizeof(SdtHeader);

    for (u32 i = 0; i < entry_count; ++i) {
        u64 table_phys = use_xsdt
            ? reinterpret_cast<u64*>(entries)[i]
            : reinterpret_cast<u32*>(entries)[i];
        auto* hdr = map<SdtHeader>(table_phys, sizeof(SdtHeader));
        if (!hdr) continue;
        if (sig_matches(hdr->sig, "APIC", 4)) {
            auto* madt = map_sdt(table_phys);
            if (!madt) {
                serial::writeln("[acpi] could not map the MADT");
                continue;
            }
            parse_madt(madt);
            serial::printf("[acpi] MADT: %u usable CPU(s), IOAPIC %s, %u interrupt override(s)\n",
                            g_info.cpu_count, g_info.ioapic_found ? "found" : "absent",
                            g_info.iso_count);
        } else if (sig_matches(hdr->sig, "HPET", 4)) {
            auto* t = map_sdt(table_phys);
            if (t) parse_hpet(t);
        } else if (sig_matches(hdr->sig, "FACP", 4)) {
            auto* t = map_sdt(table_phys);
            if (t) parse_fadt(t);
        }
    }
    serial::printf("[acpi] timers: HPET %s, PM timer %s\n", g_info.hpet_found ? "present" : "absent",
                    g_info.pm_timer_found ? "present" : "absent");
}

const Info& info() { return g_info; }

Redirection resolve_isa_irq(u8 isa_irq) {
    for (u32 i = 0; i < g_info.iso_count; ++i) {
        if (g_info.iso[i].source_irq == isa_irq) {
            u16 flags = g_info.iso[i].flags;
            u16 polarity = flags & 0x3;
            u16 trigger = (flags >> 2) & 0x3;
            return { g_info.iso[i].gsi, polarity == 3, trigger == 3 };
        }
    }
    return { isa_irq, false, false };
}

}
