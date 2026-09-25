#include <cosmos/virtio_blk.hpp>
#include <cosmos/blockdev.hpp>
#include <cosmos/pci.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/vmm.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/idt.hpp>
#include <cosmos/apic.hpp>

namespace virtioblk {

constexpr u16 VIRTIO_VENDOR = 0x1AF4;
constexpr u16 DEVICE_ID_TRANSITIONAL = 0x1001;
constexpr u16 DEVICE_ID_MODERN = 0x1042;

constexpr u8 CAP_ID_VENDOR = 0x09;
constexpr u8 CAP_ID_MSIX = 0x11;
constexpr u8 CAP_COMMON = 1;
constexpr u8 CAP_NOTIFY = 2;
constexpr u8 CAP_ISR = 3;
constexpr u8 CAP_DEVICE = 4;

constexpr u8 STATUS_ACK = 1;
constexpr u8 STATUS_DRIVER = 2;
constexpr u8 STATUS_DRIVER_OK = 4;
constexpr u8 STATUS_FEATURES_OK = 8;
constexpr u8 STATUS_FAILED = 128;

constexpr u32 FEATURE_VERSION_1 = 1u << (32 - 32);

constexpr int QSIZE = 8;

constexpr u32 BLK_T_IN = 0;
constexpr u32 BLK_T_OUT = 1;
constexpr u8 BLK_S_OK = 0;

struct PACKED VirtioPciCap {
    u8 cap_vndr, cap_next, cap_len, cfg_type, bar;
    u8 padding[3];
    u32 offset, length;
};

struct PACKED CommonCfg {
    u32 device_feature_select, device_feature;
    u32 driver_feature_select, driver_feature;
    u16 msix_config, num_queues;
    u8 device_status, config_generation;
    u16 queue_select, queue_size, queue_msix_vector, queue_enable, queue_notify_off;
    u64 queue_desc, queue_driver, queue_device;
};

struct PACKED VringDesc { u64 addr; u32 len; u16 flags; u16 next; };
constexpr u16 DESC_F_NEXT = 1;
constexpr u16 DESC_F_WRITE = 2;

struct PACKED VringAvail { u16 flags; u16 idx; u16 ring[QSIZE]; };
struct PACKED VringUsedElem { u32 id; u32 len; };
struct PACKED VringUsed { u16 flags; u16 idx; VringUsedElem ring[QSIZE]; };

struct PACKED BlkReqHeader { u32 type; u32 reserved; u64 sector; };

static u64 g_hhdm = 0;
static bool g_present = false;
static pci::Address g_addr{};

static volatile CommonCfg* g_common = nullptr;
static volatile u8* g_notify_base = nullptr;
static u32 g_notify_multiplier = 0;
static u16 g_queue_notify_off = 0;
static volatile u8* g_device_cfg = nullptr;

static VringDesc* g_desc = nullptr;
static VringAvail* g_avail = nullptr;
static VringUsed* g_used = nullptr;
static u16 g_used_seen = 0;

static BlkReqHeader* g_req_hdr = nullptr;
static u8* g_req_status = nullptr;
static u8* g_bounce = nullptr;

static volatile u32* g_msix_table = nullptr;
static bool g_msix_ready = false;
static u32 g_irq_count = 0;

static void map_bar_region(u64 phys_base) {
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
    for (u64 i = 0; i < (universe::PAGE_SIZE << order); ++i) reinterpret_cast<u8*>(v)[i] = 0;
    return v;
}
static u64 virt_to_phys(void* v) { return reinterpret_cast<u64>(v) - g_hhdm; }

static pci::Device g_found{};
static bool g_found_flag = false;

static void scan_cb(const pci::Device& d, void*) {
    if (g_found_flag) return;
    if (d.vendor_id == VIRTIO_VENDOR &&
        (d.device_id == DEVICE_ID_TRANSITIONAL || d.device_id == DEVICE_ID_MODERN)) {
        g_found = d;
        g_found_flag = true;
    }
}

bool present() { return g_present; }
u64 capacity_sectors() {
    if (!g_device_cfg) return 0;
    return *reinterpret_cast<volatile u64*>(g_device_cfg);
}
bool using_msix() { return g_msix_ready; }
u32 irq_count() { return __atomic_load_n(&g_irq_count, __ATOMIC_SEQ_CST); }

static void completion_isr(idt::Frame*) {
    apic::eoi();
    __atomic_fetch_add(&g_irq_count, 1, __ATOMIC_SEQ_CST);
}

struct HalDevice : blockdev::Device {
    bool read_sector(u64 sector, void* buf512) override;
    bool write_sector(u64 sector, const void* buf512) override;
    u64 capacity_sectors() override { return virtioblk::capacity_sectors(); }
    const char* name() override { return "virtio-blk"; }
};
static HalDevice g_hal;

bool init(u64 hhdm_offset) {
    g_hhdm = hhdm_offset;
    g_found_flag = false;
    pci::scan(&scan_cb, nullptr);
    if (!g_found_flag) {
        serial::writeln("[virtio-blk] no device found on the PCI bus");
        return false;
    }
    g_addr = g_found.addr;
    serial::printf("[virtio-blk] found at pci %u:%u.%u (device id %x)\n",
                    g_addr.bus, g_addr.device, g_addr.function, g_found.device_id);

    u16 cmd = pci::read16(g_addr, 0x04);
    pci::write16(g_addr, 0x04, cmd | 0x0006);

    u16 status = pci::read16(g_addr, 0x06);
    if (!(status & (1 << 4))) {
        serial::writeln("[virtio-blk] device has no capability list");
        return false;
    }

    u8 cap_ptr = pci::read8(g_addr, 0x34);
    while (cap_ptr) {
        u8 vndr = pci::read8(g_addr, cap_ptr);
        u8 next = pci::read8(g_addr, static_cast<u8>(cap_ptr + 1));
        if (vndr == CAP_ID_VENDOR) {
            u8 cfg_type = pci::read8(g_addr, static_cast<u8>(cap_ptr + 3));
            u8 bar = pci::read8(g_addr, static_cast<u8>(cap_ptr + 4));
            u32 off = pci::read32(g_addr, static_cast<u8>(cap_ptr + 8));
            u64 bar_phys = pci::bar_address(g_addr, bar);
            map_bar_region(bar_phys);
            u64 region_virt = g_hhdm + bar_phys + off;

            if (cfg_type == CAP_COMMON) {
                g_common = reinterpret_cast<volatile CommonCfg*>(region_virt);
            } else if (cfg_type == CAP_NOTIFY) {
                g_notify_base = reinterpret_cast<volatile u8*>(region_virt);
                g_notify_multiplier = pci::read32(g_addr, static_cast<u8>(cap_ptr + 16));
            } else if (cfg_type == CAP_DEVICE) {
                g_device_cfg = reinterpret_cast<volatile u8*>(region_virt);
            }
        } else if (vndr == CAP_ID_MSIX) {
            u16 msg_ctrl = pci::read16(g_addr, static_cast<u8>(cap_ptr + 2));
            u32 table_reg = pci::read32(g_addr, static_cast<u8>(cap_ptr + 4));
            u8 bir = static_cast<u8>(table_reg & 0x7);
            u32 table_off = table_reg & ~0x7u;
            u64 bar_phys = pci::bar_address(g_addr, bir);
            map_bar_region(bar_phys);
            g_msix_table = reinterpret_cast<volatile u32*>(g_hhdm + bar_phys + table_off);

            idt::set_handler(idt::VEC_VIRTIO_BLK, &completion_isr);
            g_msix_table[0] = 0xFEE00000u | (apic::id() << 12);
            g_msix_table[1] = 0;
            g_msix_table[2] = idt::VEC_VIRTIO_BLK;
            g_msix_table[3] = 0;

            msg_ctrl |= (1u << 15);
            msg_ctrl &= static_cast<u16>(~(1u << 14));
            pci::write16(g_addr, static_cast<u8>(cap_ptr + 2), msg_ctrl);
            g_msix_ready = true;
        }
        cap_ptr = next;
    }

    if (!g_common || !g_notify_base || !g_device_cfg) {
        serial::writeln("[virtio-blk] missing a required capability region");
        return false;
    }

    g_common->device_status = 0;
    while (g_common->device_status != 0) {}
    g_common->device_status |= STATUS_ACK;
    g_common->device_status |= STATUS_DRIVER;

    g_common->driver_feature_select = 1;
    g_common->driver_feature = FEATURE_VERSION_1;
    g_common->driver_feature_select = 0;
    g_common->driver_feature = 0;

    g_common->device_status |= STATUS_FEATURES_OK;
    if (!(g_common->device_status & STATUS_FEATURES_OK)) {
        serial::writeln("[virtio-blk] device rejected VIRTIO_F_VERSION_1");
        return false;
    }

    g_common->queue_select = 0;
    u16 max_qsize = g_common->queue_size;
    u16 qsize = (max_qsize < QSIZE) ? max_qsize : QSIZE;
    if (qsize == 0) { serial::writeln("[virtio-blk] queue 0 unavailable"); return false; }

    g_desc = static_cast<VringDesc*>(alloc_pages(sizeof(VringDesc) * QSIZE));
    g_avail = static_cast<VringAvail*>(alloc_pages(sizeof(VringAvail)));
    g_used = static_cast<VringUsed*>(alloc_pages(sizeof(VringUsed)));

    g_common->queue_size = qsize;
    g_common->queue_desc = virt_to_phys(g_desc);
    g_common->queue_driver = virt_to_phys(g_avail);
    g_common->queue_device = virt_to_phys(g_used);
    g_queue_notify_off = g_common->queue_notify_off;

    if (g_msix_ready) {
        g_common->queue_msix_vector = 0;
        if (g_common->queue_msix_vector != 0) g_msix_ready = false;
    }

    g_common->queue_enable = 1;

    g_common->device_status |= STATUS_DRIVER_OK;

    g_req_hdr = static_cast<BlkReqHeader*>(alloc_pages(sizeof(BlkReqHeader)));
    g_req_status = static_cast<u8*>(alloc_pages(1));
    g_bounce = static_cast<u8*>(alloc_pages(512));

    g_present = true;
    serial::printf("[virtio-blk] ready, capacity=%lu sectors, completion mode=%s\n",
                    capacity_sectors(), g_msix_ready ? "MSI-X" : "polled");
    if (g_msix_ready) cpu::sti();
    blockdev::register_device(&g_hal);
    return true;
}

static bool do_request(u64 sector, bool write) {
    if (!g_present) return false;

    g_req_hdr->type = write ? BLK_T_OUT : BLK_T_IN;
    g_req_hdr->reserved = 0;
    g_req_hdr->sector = sector;
    *g_req_status = 0xFF;

    g_desc[0] = { virt_to_phys(g_req_hdr), sizeof(BlkReqHeader), DESC_F_NEXT, 1 };
    g_desc[1] = { virt_to_phys(g_bounce), 512, static_cast<u16>(DESC_F_NEXT | (write ? 0 : DESC_F_WRITE)), 2 };
    g_desc[2] = { virt_to_phys(g_req_status), 1, DESC_F_WRITE, 0 };

    u16 slot = g_avail->idx % QSIZE;
    g_avail->ring[slot] = 0;
    __sync_synchronize();
    g_avail->idx = static_cast<u16>(g_avail->idx + 1);
    __sync_synchronize();

    volatile u16* notify = reinterpret_cast<volatile u16*>(
        g_notify_base + static_cast<u64>(g_queue_notify_off) * g_notify_multiplier);
    *notify = 0;

    u64 waits = 0;
    while (g_used->idx == g_used_seen) {
        if (g_msix_ready) cpu::halt(); else cpu::io_wait();
        if (++waits > 20000000ull) {
            serial::writeln("[virtio-blk] request timed out");
            return false;
        }
    }
    g_used_seen = g_used->idx;

    return *g_req_status == BLK_S_OK;
}

bool read_sector(u64 sector, void* buf512) {
    if (!do_request(sector, false)) return false;
    for (int i = 0; i < 512; ++i) static_cast<u8*>(buf512)[i] = g_bounce[i];
    return true;
}
bool write_sector(u64 sector, const void* buf512) {
    for (int i = 0; i < 512; ++i) g_bounce[i] = static_cast<const u8*>(buf512)[i];
    return do_request(sector, true);
}

bool HalDevice::read_sector(u64 sector, void* buf512) { return virtioblk::read_sector(sector, buf512); }
bool HalDevice::write_sector(u64 sector, const void* buf512) { return virtioblk::write_sector(sector, buf512); }

}
