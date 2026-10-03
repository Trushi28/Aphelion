#include <cosmos/ahci.hpp>
#include <cosmos/blockdev.hpp>
#include <cosmos/pci.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/vmm.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/idt.hpp>
#include <cosmos/apic.hpp>

namespace ahci {

constexpr u8 CLASS_STORAGE = 0x01;
constexpr u8 SUBCLASS_SATA = 0x06;
constexpr u8 PROGIF_AHCI = 0x01;

constexpr u32 SATA_SIG_ATA = 0x00000101;

constexpr u32 GHC_AE = 1u << 31;
constexpr u32 GHC_HR = 1u << 0;

constexpr u32 PORT_CMD_ST = 1u << 0;
constexpr u32 PORT_CMD_FRE = 1u << 4;
constexpr u32 PORT_CMD_FR = 1u << 14;
constexpr u32 PORT_CMD_CR = 1u << 15;

constexpr u32 PORT_TFD_BSY = 1u << 7;
constexpr u32 PORT_TFD_DRQ = 1u << 3;

constexpr u32 PORT_IS_TFES = 1u << 30;

constexpr u32 GHC_IE = 1u << 1;
constexpr u32 PORT_IE_DHRE = 1u << 0;
constexpr u32 PORT_IE_SDBE = 1u << 3;
constexpr u32 PORT_IE_TFEE = 1u << 30;

constexpr u32 HBA_CAP_SNCQ = 1u << 30;
constexpr u32 MAX_INFLIGHT = 16;
constexpr u64 CMD_TABLE_STRIDE = 256;

constexpr u8 CAP_ID_MSI = 0x05;

constexpr u8 ATA_CMD_IDENTIFY = 0xEC;
constexpr u8 ATA_CMD_READ_DMA_EXT = 0x25;
constexpr u8 ATA_CMD_WRITE_DMA_EXT = 0x35;
constexpr u8 ATA_CMD_READ_FPDMA = 0x60;
constexpr u8 ATA_CMD_WRITE_FPDMA = 0x61;

struct PACKED HbaPort {
    u32 clb;
    u32 clbu;
    u32 fb;
    u32 fbu;
    u32 is;
    u32 ie;
    u32 cmd;
    u32 reserved0;
    u32 tfd;
    u32 sig;
    u32 ssts;
    u32 sctl;
    u32 serr;
    u32 sact;
    u32 ci;
    u32 sntf;
    u32 fbs;
    u32 reserved1[11];
    u32 vendor[4];
};

struct PACKED HbaMem {
    u32 cap;
    u32 ghc;
    u32 is;
    u32 pi;
    u32 vs;
    u32 ccc_ctl;
    u32 ccc_pts;
    u32 em_loc;
    u32 em_ctl;
    u32 cap2;
    u32 bohc;
};

struct PACKED FisRegH2D {
    u8 fis_type;
    u8 pmport_c;
    u8 command;
    u8 featurel;
    u8 lba0, lba1, lba2;
    u8 device;
    u8 lba3, lba4, lba5;
    u8 featureh;
    u8 countl, counth;
    u8 icc;
    u8 control;
    u8 reserved[4];
};
static_assert(sizeof(FisRegH2D) == 20, "FIS_REG_H2D must be 20 bytes");

struct PACKED HbaCmdHeader {
    u8 cfl_a_w_p;
    u8 r_b_c_pmp;
    u16 prdtl;
    u32 prdbc;
    u32 ctba;
    u32 ctbau;
    u32 reserved[4];
};
static_assert(sizeof(HbaCmdHeader) == 32, "command header must be 32 bytes");

struct PACKED HbaPrdtEntry {
    u32 dba;
    u32 dbau;
    u32 reserved0;
    u32 dbc_i;
};

struct PACKED HbaCmdTable {
    u8 cfis[64];
    u8 acmd[16];
    u8 reserved[48];
    HbaPrdtEntry prdt_entry[1];
};

static u64 g_hhdm = 0;
static bool g_present = false;
static volatile HbaMem* g_hba = nullptr;
static volatile HbaPort* g_port = nullptr;
static HbaCmdHeader* g_cmd_list = nullptr;
static u8* g_cmd_table = nullptr;
static u8* g_bounce = nullptr;
static u8* g_bounce_pool = nullptr;
static bool g_ncq_ready = false;
static u32 g_max_inflight = 1;
static u64 g_capacity_sectors = 0;
static int g_port_index = -1;
static bool g_msi_ready = false;
static u32 g_irq_count = 0;
static u32 g_is_latched = 0;

static void map_region(u64 phys_base) {
    u64 page_base = phys_base & ~0xFFFull;
    for (u64 off = 0; off < 0x10000; off += 0x1000)
        constellation::map_4k(g_hhdm + page_base + off, page_base + off,
                               constellation::WRITABLE | constellation::NO_CACHE);
}

static void* alloc_pages(u64 bytes) {
    int order = 0;
    while ((universe::PAGE_SIZE << order) < bytes) ++order;
    u64 phys = universe::alloc(order);
    void* v = reinterpret_cast<void*>(g_hhdm + phys);
    __builtin_memset(v, 0, universe::PAGE_SIZE << order);
    return v;
}
static u64 virt_to_phys(void* v) { return reinterpret_cast<u64>(v) - g_hhdm; }
static HbaCmdTable* table_for(u32 slot) {
    return reinterpret_cast<HbaCmdTable*>(g_cmd_table + static_cast<u64>(slot) * CMD_TABLE_STRIDE);
}
static u8* bounce_for(u32 slot) { return g_bounce_pool + static_cast<u64>(slot) * 512; }

static pci::Device g_found{};
static bool g_found_flag = false;

static void scan_cb(const pci::Device& d, void*) {
    if (g_found_flag) return;
    if (d.class_code == CLASS_STORAGE && d.subclass == SUBCLASS_SATA && d.prog_if == PROGIF_AHCI) {
        g_found = d;
        g_found_flag = true;
    }
}

bool present() { return g_present; }
u64 capacity_sectors() { return g_capacity_sectors; }
bool using_msi() { return g_msi_ready; }
u32 irq_count() { return __atomic_load_n(&g_irq_count, __ATOMIC_SEQ_CST); }

static void completion_isr(idt::Frame*) {
    apic::eoi();
    u32 port_is = g_port->is;
    g_port->is = port_is;
    g_hba->is = 1u << g_port_index;
    __atomic_fetch_or(&g_is_latched, port_is, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&g_irq_count, 1, __ATOMIC_SEQ_CST);
}

static bool task_file_error() {
    return ((g_port->is | __atomic_load_n(&g_is_latched, __ATOMIC_SEQ_CST)) & PORT_IS_TFES) != 0;
}

static bool wait_not_busy(u64 spins_max) {
    u64 spins = 0;
    while (g_port->tfd & (PORT_TFD_BSY | PORT_TFD_DRQ)) {
        cpu::io_wait();
        if (++spins > spins_max) return false;
    }
    return true;
}

static bool run_command(u8 command, u64 lba, u16 count, void* buf, bool write) {
    if (!wait_not_busy(10000000ull)) return false;

    g_port->is = 0xFFFFFFFFu;
    __atomic_store_n(&g_is_latched, 0u, __ATOMIC_SEQ_CST);

    HbaCmdHeader* hdr = &g_cmd_list[0];
    hdr->cfl_a_w_p = static_cast<u8>((sizeof(FisRegH2D) / 4) | (write ? (1u << 6) : 0));
    hdr->r_b_c_pmp = 0;
    hdr->prdtl = 1;
    hdr->prdbc = 0;

    HbaCmdTable* tbl = table_for(0);
    for (auto& b : tbl->cfis) b = 0;

    tbl->prdt_entry[0].dba = static_cast<u32>(virt_to_phys(buf));
    tbl->prdt_entry[0].dbau = static_cast<u32>(virt_to_phys(buf) >> 32);
    tbl->prdt_entry[0].dbc_i = ((static_cast<u32>(count) * 512u) - 1) | (1u << 31);

    auto* fis = reinterpret_cast<FisRegH2D*>(tbl->cfis);
    fis->fis_type = 0x27;
    fis->pmport_c = 1u << 7;
    fis->command = command;
    fis->lba0 = static_cast<u8>(lba);
    fis->lba1 = static_cast<u8>(lba >> 8);
    fis->lba2 = static_cast<u8>(lba >> 16);
    fis->device = 1u << 6;
    fis->lba3 = static_cast<u8>(lba >> 24);
    fis->lba4 = static_cast<u8>(lba >> 32);
    fis->lba5 = static_cast<u8>(lba >> 40);
    fis->countl = static_cast<u8>(count);
    fis->counth = static_cast<u8>(count >> 8);

    __sync_synchronize();
    g_port->ci = 1u;

    u64 spins = 0;
    u64 irq_flags = cpu::irq_save();
    bool ok = true;
    for (;;) {
        if (!(g_port->ci & 1u)) break;
        if (task_file_error()) { ok = false; break; }
        if (g_msi_ready) { cpu::sti_halt(); cpu::cli(); } else cpu::io_wait();
        if (++spins > 20000000ull) {
            serial::writeln("[ahci] command timed out");
            ok = false;
            break;
        }
    }
    __sync_synchronize();
    cpu::irq_restore(irq_flags);
    return ok && !task_file_error();
}

static bool recover_port() {
    g_port->cmd &= ~PORT_CMD_ST;
    u64 spins = 0;
    while (g_port->cmd & PORT_CMD_CR) {
        cpu::io_wait();
        if (++spins > 1000000ull) return false;
    }
    g_port->serr = 0xFFFFFFFFu;
    g_port->is = 0xFFFFFFFFu;
    __atomic_store_n(&g_is_latched, 0u, __ATOMIC_SEQ_CST);
    g_port->cmd |= PORT_CMD_ST;
    return true;
}

static void submit_ncq(u32 slot, u64 lba, bool write) {
    HbaCmdHeader* hdr = &g_cmd_list[slot];
    hdr->cfl_a_w_p = static_cast<u8>((sizeof(FisRegH2D) / 4) | (write ? (1u << 6) : 0));
    hdr->r_b_c_pmp = 0;
    hdr->prdtl = 1;
    hdr->prdbc = 0;

    HbaCmdTable* tbl = table_for(slot);
    for (auto& b : tbl->cfis) b = 0;

    u64 buf_phys = virt_to_phys(bounce_for(slot));
    tbl->prdt_entry[0].dba = static_cast<u32>(buf_phys);
    tbl->prdt_entry[0].dbau = static_cast<u32>(buf_phys >> 32);
    tbl->prdt_entry[0].dbc_i = (512u - 1) | (1u << 31);

    auto* fis = reinterpret_cast<FisRegH2D*>(tbl->cfis);
    fis->fis_type = 0x27;
    fis->pmport_c = 1u << 7;
    fis->command = write ? ATA_CMD_WRITE_FPDMA : ATA_CMD_READ_FPDMA;
    fis->featurel = 1;
    fis->lba0 = static_cast<u8>(lba);
    fis->lba1 = static_cast<u8>(lba >> 8);
    fis->lba2 = static_cast<u8>(lba >> 16);
    fis->device = 1u << 6;
    fis->lba3 = static_cast<u8>(lba >> 24);
    fis->lba4 = static_cast<u8>(lba >> 32);
    fis->lba5 = static_cast<u8>(lba >> 40);
    fis->featureh = 0;
    fis->countl = static_cast<u8>(slot << 3);
    fis->counth = 0;
}

static bool run_ncq_batch(u64 start_sector, u32 n, bool write) {
    if (!wait_not_busy(10000000ull)) return false;

    g_port->is = 0xFFFFFFFFu;
    __atomic_store_n(&g_is_latched, 0u, __ATOMIC_SEQ_CST);

    u32 mask = (n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1u);
    for (u32 i = 0; i < n; ++i) submit_ncq(i, start_sector + i, write);

    __sync_synchronize();
    g_port->sact = mask;
    __sync_synchronize();
    g_port->ci = mask;

    u64 spins = 0;
    u64 irq_flags = cpu::irq_save();
    bool ok = true;
    for (;;) {
        if (!((g_port->sact | g_port->ci) & mask)) break;
        if (task_file_error()) { ok = false; break; }
        if (g_msi_ready) { cpu::sti_halt(); cpu::cli(); } else cpu::io_wait();
        if (++spins > 20000000ull) {
            serial::writeln("[ahci] NCQ batch timed out");
            ok = false;
            break;
        }
    }
    __sync_synchronize();
    cpu::irq_restore(irq_flags);

    if (!ok || task_file_error()) {
        recover_port();
        return false;
    }
    return true;
}

static bool identify(u8* out512) {
    return run_command(ATA_CMD_IDENTIFY, 0, 1, out512, false);
}

bool read_sector(u64 sector, void* buf512) {
    if (!g_present) return false;
    if (!run_command(ATA_CMD_READ_DMA_EXT, sector, 1, g_bounce, false)) return false;
    __builtin_memcpy(buf512, g_bounce, 512);
    return true;
}
bool write_sector(u64 sector, const void* buf512) {
    if (!g_present) return false;
    __builtin_memcpy(g_bounce, buf512, 512);
    return run_command(ATA_CMD_WRITE_DMA_EXT, sector, 1, g_bounce, true);
}

struct HalDevice : blockdev::Device {
    bool read_sector(u64 sector, void* buf512) override { return ahci::read_sector(sector, buf512); }
    bool write_sector(u64 sector, const void* buf512) override { return ahci::write_sector(sector, buf512); }
    bool read_sectors(u64 start_sector, u64 count, void* buf) override {
        if (!g_present || start_sector + count > g_capacity_sectors) return false;
        u8* dst = static_cast<u8*>(buf);
        if (!g_ncq_ready) {
            for (u64 i = 0; i < count; ++i)
                if (!ahci::read_sector(start_sector + i, dst + i * 512)) return false;
            return true;
        }
        while (count > 0) {
            u32 batch = static_cast<u32>(count < g_max_inflight ? count : g_max_inflight);
            if (!run_ncq_batch(start_sector, batch, false)) return false;
            for (u32 i = 0; i < batch; ++i)
                __builtin_memcpy(dst + static_cast<u64>(i) * 512, bounce_for(i), 512);
            dst += static_cast<u64>(batch) * 512;
            start_sector += batch;
            count -= batch;
        }
        return true;
    }
    bool write_sectors(u64 start_sector, u64 count, const void* buf) override {
        if (!g_present || start_sector + count > g_capacity_sectors) return false;
        const u8* src = static_cast<const u8*>(buf);
        if (!g_ncq_ready) {
            for (u64 i = 0; i < count; ++i)
                if (!ahci::write_sector(start_sector + i, src + i * 512)) return false;
            return true;
        }
        while (count > 0) {
            u32 batch = static_cast<u32>(count < g_max_inflight ? count : g_max_inflight);
            for (u32 i = 0; i < batch; ++i)
                __builtin_memcpy(bounce_for(i), src + static_cast<u64>(i) * 512, 512);
            if (!run_ncq_batch(start_sector, batch, true)) return false;
            src += static_cast<u64>(batch) * 512;
            start_sector += batch;
            count -= batch;
        }
        return true;
    }
    u64 capacity_sectors() override { return ahci::capacity_sectors(); }
    const char* name() override { return "ahci"; }
};
static HalDevice g_hal;

static bool rebase_port() {
    g_port->cmd &= ~(PORT_CMD_ST | PORT_CMD_FRE);
    u64 spins = 0;
    while (g_port->cmd & (PORT_CMD_FR | PORT_CMD_CR)) {
        cpu::io_wait();
        if (++spins > 1000000ull) return false;
    }

    g_cmd_list = static_cast<HbaCmdHeader*>(alloc_pages(1024));
    void* fis_base = alloc_pages(256);
    g_cmd_table = static_cast<u8*>(alloc_pages(CMD_TABLE_STRIDE * MAX_INFLIGHT));

    u64 clb_phys = virt_to_phys(g_cmd_list);
    g_port->clb = static_cast<u32>(clb_phys);
    g_port->clbu = static_cast<u32>(clb_phys >> 32);

    u64 fb_phys = virt_to_phys(fis_base);
    g_port->fb = static_cast<u32>(fb_phys);
    g_port->fbu = static_cast<u32>(fb_phys >> 32);

    for (u32 slot = 0; slot < MAX_INFLIGHT; ++slot) {
        u64 ctba_phys = virt_to_phys(table_for(slot));
        g_cmd_list[slot].ctba = static_cast<u32>(ctba_phys);
        g_cmd_list[slot].ctbau = static_cast<u32>(ctba_phys >> 32);
    }

    g_port->serr = 0xFFFFFFFFu;
    g_port->is = 0xFFFFFFFFu;
    g_port->ie = PORT_IE_DHRE | PORT_IE_SDBE | PORT_IE_TFEE;

    __sync_synchronize();
    g_port->cmd |= PORT_CMD_FRE;
    g_port->cmd |= PORT_CMD_ST;
    return true;
}

bool init(u64 hhdm_offset) {
    g_hhdm = hhdm_offset;
    g_found_flag = false;
    pci::scan(&scan_cb, nullptr);
    if (!g_found_flag) {
        serial::writeln("[ahci] no AHCI controller found on the PCI bus");
        return false;
    }
    serial::printf("[ahci] found at pci %u:%u.%u\n",
                    g_found.addr.bus, g_found.addr.device, g_found.addr.function);

    u16 cmd = pci::read16(g_found.addr, 0x04);
    pci::write16(g_found.addr, 0x04, cmd | 0x0006);

    u16 status = pci::read16(g_found.addr, 0x06);
    if (status & (1u << 4)) {
        u8 cap_ptr = pci::read8(g_found.addr, 0x34);
        while (cap_ptr) {
            u8 cap_id = pci::read8(g_found.addr, cap_ptr);
            u8 next = pci::read8(g_found.addr, static_cast<u8>(cap_ptr + 1));
            if (cap_id == CAP_ID_MSI) {
                u16 msg_ctrl = pci::read16(g_found.addr, static_cast<u8>(cap_ptr + 2));
                bool addr64 = (msg_ctrl & (1u << 7)) != 0;
                u32 addr_lo = 0xFEE00000u | (apic::id() << 12);
                pci::write32(g_found.addr, static_cast<u8>(cap_ptr + 4), addr_lo);
                u8 data_off = static_cast<u8>(cap_ptr + (addr64 ? 12 : 8));
                if (addr64) pci::write32(g_found.addr, static_cast<u8>(cap_ptr + 8), 0);
                pci::write16(g_found.addr, data_off, idt::VEC_AHCI);
                msg_ctrl = static_cast<u16>((msg_ctrl & ~(0x7u << 4)) | 1u);
                pci::write16(g_found.addr, static_cast<u8>(cap_ptr + 2), msg_ctrl);
                idt::set_handler(idt::VEC_AHCI, &completion_isr);
                g_msi_ready = true;
                break;
            }
            cap_ptr = next;
        }
    }

    u64 abar_phys = pci::bar_address(g_found.addr, 5);
    map_region(abar_phys);
    g_hba = reinterpret_cast<volatile HbaMem*>(g_hhdm + abar_phys);

    g_hba->ghc |= GHC_AE;

    u32 pi = g_hba->pi;
    int found_port = -1;
    for (int i = 0; i < 32 && found_port < 0; ++i) {
        if (!(pi & (1u << i))) continue;
        auto* port = reinterpret_cast<volatile HbaPort*>(
            reinterpret_cast<volatile u8*>(g_hba) + 0x100 + i * 0x80);
        u8 det = static_cast<u8>(port->ssts & 0xF);
        u8 ipm = static_cast<u8>((port->ssts >> 8) & 0xF);
        if (det == 3 && ipm == 1 && port->sig == SATA_SIG_ATA) found_port = i;
    }
    if (found_port < 0) {
        serial::writeln("[ahci] no active SATA disk found on any implemented port");
        return false;
    }
    serial::printf("[ahci] using port %d\n", found_port);
    g_port_index = found_port;
    g_port = reinterpret_cast<volatile HbaPort*>(
        reinterpret_cast<volatile u8*>(g_hba) + 0x100 + found_port * 0x80);

    if (!rebase_port()) {
        serial::writeln("[ahci] port rebase timed out");
        return false;
    }

    if (g_msi_ready) g_hba->ghc |= GHC_IE;

    g_bounce = static_cast<u8*>(alloc_pages(512));
    g_bounce_pool = static_cast<u8*>(alloc_pages(512ull * MAX_INFLIGHT));

    u8 id_buf[512];
    if (!identify(id_buf)) {
        serial::writeln("[ahci] IDENTIFY DEVICE failed");
        return false;
    }
    auto* words = reinterpret_cast<u16*>(id_buf);
    bool lba48 = (words[83] & (1u << 10)) != 0;
    if (lba48) {
        u64 lo = words[100] | (static_cast<u64>(words[101]) << 16);
        u64 hi = static_cast<u64>(words[102]) | (static_cast<u64>(words[103]) << 16);
        g_capacity_sectors = lo | (hi << 32);
    } else {
        g_capacity_sectors = words[60] | (static_cast<u64>(words[61]) << 16);
    }

    bool hba_ncq = (g_hba->cap & HBA_CAP_SNCQ) != 0;
    bool dev_ncq = (words[76] & (1u << 8)) != 0;
    u32 dev_depth = static_cast<u32>(words[75] & 0x1F) + 1;
    if (hba_ncq && dev_ncq) {
        g_max_inflight = dev_depth < MAX_INFLIGHT ? dev_depth : MAX_INFLIGHT;
        g_ncq_ready = g_max_inflight > 1;
    }

    g_present = true;
    serial::printf("[ahci] ready, capacity=%lu sectors, completion mode=%s, %u request(s) in flight (%s)\n",
                    g_capacity_sectors, g_msi_ready ? "MSI" : "polled",
                    g_ncq_ready ? g_max_inflight : 1u, g_ncq_ready ? "NCQ" : "single-command");
    blockdev::register_device(&g_hal);
    return true;
}

}
