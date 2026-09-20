#include <cosmos/acpi.hpp>
#include <cosmos/serial.hpp>

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

static Info g_info;
static u64 g_hhdm = 0;

template <typename T> static T* phys(u64 addr) {
    return reinterpret_cast<T*>(g_hhdm + addr);
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
            default: break;
        }
        p += e->length;
    }
}

void init(u64 rsdp_phys, u64 hhdm_offset) {
    g_hhdm = hhdm_offset;
    g_info = Info{};

    auto* rsdp = phys<Rsdp>(rsdp_phys);
    bool use_xsdt = rsdp->revision >= 2 && rsdp->xsdt_addr != 0;

    SdtHeader* root = use_xsdt ? phys<SdtHeader>(rsdp->xsdt_addr)
                                : phys<SdtHeader>(rsdp->rsdt_addr);

    u32 entry_count = (root->length - sizeof(SdtHeader)) / (use_xsdt ? 8 : 4);
    u8* entries = reinterpret_cast<u8*>(root) + sizeof(SdtHeader);

    for (u32 i = 0; i < entry_count; ++i) {
        u64 table_phys = use_xsdt
            ? reinterpret_cast<u64*>(entries)[i]
            : reinterpret_cast<u32*>(entries)[i];
        auto* hdr = phys<SdtHeader>(table_phys);
        if (sig_matches(hdr->sig, "APIC", 4)) {
            parse_madt(hdr);
            serial::printf("[acpi] MADT: %u usable CPU(s), IOAPIC %s\n",
                            g_info.cpu_count, g_info.ioapic_found ? "found" : "absent");
        }
    }
}

const Info& info() { return g_info; }

}
