#include <cosmos/nvme.hpp>
#include <cosmos/blockdev.hpp>
#include <cosmos/pci.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/vmm.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/idt.hpp>
#include <cosmos/apic.hpp>
#include <cosmos/orbital.hpp>

namespace nvme {

constexpr u8 CLASS_STORAGE = 0x01;
constexpr u8 SUBCLASS_NVM = 0x08;
constexpr u8 PROGIF_NVME = 0x02;

constexpr u8 CAP_ID_MSI = 0x05;
constexpr u8 CAP_ID_MSIX = 0x11;

constexpr u32 REG_CAP = 0x00;
constexpr u32 REG_VS = 0x08;
constexpr u32 REG_CC = 0x14;
constexpr u32 REG_CSTS = 0x1C;
constexpr u32 REG_AQA = 0x24;
constexpr u32 REG_ASQ = 0x28;
constexpr u32 REG_ACQ = 0x30;
constexpr u32 REG_DOORBELL = 0x1000;

constexpr u32 CC_EN = 1u << 0;
constexpr u32 CC_IOSQES = 6u << 16;
constexpr u32 CC_IOCQES = 4u << 20;
constexpr u32 CSTS_RDY = 1u << 0;
constexpr u32 CSTS_CFS = 1u << 1;

constexpr u8 ADMIN_CREATE_SQ = 0x01;
constexpr u8 ADMIN_CREATE_CQ = 0x05;
constexpr u8 ADMIN_IDENTIFY = 0x06;
constexpr u8 IO_WRITE = 0x01;
constexpr u8 IO_READ = 0x02;

constexpr u32 CNS_NAMESPACE = 0x00;
constexpr u32 CNS_CONTROLLER = 0x01;
constexpr u32 CNS_ACTIVE_LIST = 0x02;

constexpr u16 ADMIN_DEPTH = 8;
constexpr u32 MAX_INFLIGHT = 16;
constexpr u64 MAP_BYTES = 0x10000;
constexpr u64 SPINS_PER_HALF_SECOND = 500000;
constexpr u64 COMMAND_SPINS = 20000000;

struct PACKED Sqe {
    u32 cdw0;
    u32 nsid;
    u32 cdw2;
    u32 cdw3;
    u64 mptr;
    u64 prp1;
    u64 prp2;
    u32 cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};
static_assert(sizeof(Sqe) == 64, "submission entry must be 64 bytes");

struct PACKED Cqe {
    u32 dw0;
    u32 dw1;
    u16 sq_head;
    u16 sq_id;
    u16 cid;
    u16 status;
};
static_assert(sizeof(Cqe) == 16, "completion entry must be 16 bytes");

struct Queue {
    Sqe* sq;
    volatile Cqe* cq;
    volatile u32* sq_doorbell;
    volatile u32* cq_doorbell;
    u16 depth;
    u16 sq_tail;
    u16 cq_head;
    u16 phase;
};

enum class IrqMode : u8 { Polled, Msi, MsiX };

static u64 g_hhdm = 0;
static bool g_present = false;
static pci::Address g_addr{};
static volatile u8* g_regs = nullptr;
static u32 g_stride = 4;
static u16 g_depth = 0;
static u32 g_io_max_inflight = 1;

static Queue g_admin{};
static Queue g_io{};

static u8* g_ident = nullptr;
static u8* g_admin_bounce = nullptr;
static u8* g_io_bounce_pool = nullptr;
static bool g_slot_done[MAX_INFLIGHT];
static u16 g_slot_status[MAX_INFLIGHT];

static u32 g_nsid = 1;
static u32 g_lba_shift = 9;
static u64 g_capacity_sectors = 0;

static IrqMode g_mode = IrqMode::Polled;
static bool g_use_irq = false;
static u32 g_irq_count = 0;
static u16 g_last_status = 0;

static pci::Device g_found{};
static bool g_found_flag = false;

static inline u32 reg32(u32 off) { return *reinterpret_cast<volatile u32*>(g_regs + off); }
static inline void wreg32(u32 off, u32 v) { *reinterpret_cast<volatile u32*>(g_regs + off) = v; }
static inline u64 reg64(u32 off) {
    return static_cast<u64>(reg32(off)) | (static_cast<u64>(reg32(off + 4)) << 32);
}
static inline void wreg64(u32 off, u64 v) {
    wreg32(off, static_cast<u32>(v));
    wreg32(off + 4, static_cast<u32>(v >> 32));
}

static u32 rd32(const u8* p) { u32 v; __builtin_memcpy(&v, p, sizeof(v)); return v; }
static u64 rd64(const u8* p) { u64 v; __builtin_memcpy(&v, p, sizeof(v)); return v; }

static orbital::Mutex g_dev_lock;

struct DevGuard {
    DevGuard() { g_dev_lock.lock(); }
    ~DevGuard() { g_dev_lock.unlock(); }
};

static void map_region(u64 phys_base) {
    u64 page_base = phys_base & ~0xFFFull;
    for (u64 off = 0; off < MAP_BYTES; off += 0x1000)
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
static u8* io_bounce_for(u32 slot) { return g_io_bounce_pool + static_cast<u64>(slot) * 4096; }

static void scan_cb(const pci::Device& d, void*) {
    if (g_found_flag) return;
    if (d.class_code == CLASS_STORAGE && d.subclass == SUBCLASS_NVM && d.prog_if == PROGIF_NVME) {
        g_found = d;
        g_found_flag = true;
    }
}

bool present() { return g_present; }
u64 capacity_sectors() { return g_capacity_sectors; }
u32 irq_count() { return __atomic_load_n(&g_irq_count, __ATOMIC_SEQ_CST); }

const char* completion_mode() {
    if (g_mode == IrqMode::Polled || !g_use_irq) return "polled";
    return g_mode == IrqMode::MsiX ? "MSI-X" : "MSI";
}

static void completion_isr(idt::Frame*) {
    apic::eoi();
    __atomic_fetch_add(&g_irq_count, 1, __ATOMIC_SEQ_CST);
}

static bool wait_ready(bool want, u64 limit) {
    for (u64 i = 0; i < limit; ++i) {
        u32 csts = reg32(REG_CSTS);
        if (((csts & CSTS_RDY) != 0) == want) return true;
        if (want && (csts & CSTS_CFS)) return false;
        cpu::io_wait();
    }
    return false;
}

static bool execute(Queue& q, Sqe& cmd) {
    static u16 next_cid = 1;
    u16 cid = next_cid++;
    if (next_cid == 0) next_cid = 1;
    cmd.cdw0 = (cmd.cdw0 & 0xFFFFu) | (static_cast<u32>(cid) << 16);

    q.sq[q.sq_tail] = cmd;
    q.sq_tail = static_cast<u16>((q.sq_tail + 1) % q.depth);
    __sync_synchronize();
    *q.sq_doorbell = q.sq_tail;

    u64 irq_flags = cpu::irq_save();
    bool done = false;
    for (u64 spins = 0; spins < COMMAND_SPINS; ++spins) {
        if ((q.cq[q.cq_head].status & 1u) == q.phase) { done = true; break; }
        if (g_use_irq) { cpu::sti_halt(); cpu::cli(); } else cpu::io_wait();
    }
    if (!done) {
        cpu::irq_restore(irq_flags);
        serial::writeln("[nvme] command timed out");
        return false;
    }
    __sync_synchronize();
    u16 status = q.cq[q.cq_head].status;
    u16 got_cid = q.cq[q.cq_head].cid;
    q.cq_head = static_cast<u16>((q.cq_head + 1) % q.depth);
    if (q.cq_head == 0) q.phase = static_cast<u16>(q.phase ^ 1u);
    *q.cq_doorbell = q.cq_head;
    cpu::irq_restore(irq_flags);

    g_last_status = static_cast<u16>(status >> 1);
    if (got_cid != cid) {
        serial::printf("[nvme] completion id mismatch: expected %u, got %u\n",
                        static_cast<u32>(cid), static_cast<u32>(got_cid));
        return false;
    }
    return g_last_status == 0;
}

static bool admin(u8 opcode, u32 nsid, u64 prp1, u32 cdw10, u32 cdw11) {
    Sqe cmd{};
    cmd.cdw0 = opcode;
    cmd.nsid = nsid;
    cmd.prp1 = prp1;
    cmd.cdw10 = cdw10;
    cmd.cdw11 = cdw11;
    return execute(g_admin, cmd);
}

static bool identify(u32 cns, u32 nsid) {
    for (u64 i = 0; i < 4096; ++i) g_ident[i] = 0;
    return admin(ADMIN_IDENTIFY, nsid, virt_to_phys(g_ident), cns, 0);
}

static void submit_io(u32 slot, u64 lba, bool write) {
    u16 cid = static_cast<u16>(slot + 1);
    Sqe& cmd = g_io.sq[g_io.sq_tail];
    cmd = Sqe{};
    cmd.cdw0 = static_cast<u32>((write ? IO_WRITE : IO_READ) | (static_cast<u32>(cid) << 16));
    cmd.nsid = g_nsid;
    cmd.prp1 = virt_to_phys(io_bounce_for(slot));
    cmd.cdw10 = static_cast<u32>(lba);
    cmd.cdw11 = static_cast<u32>(lba >> 32);
    g_slot_done[slot] = false;
    g_io.sq_tail = static_cast<u16>((g_io.sq_tail + 1) % g_io.depth);
}

static void ring_io_sq_doorbell() {
    __sync_synchronize();
    *g_io.sq_doorbell = g_io.sq_tail;
}

static bool drain_io(u32 pending) {
    u64 waits = 0;
    u64 irq_flags = cpu::irq_save();
    while (pending > 0) {
        if ((g_io.cq[g_io.cq_head].status & 1u) != g_io.phase) {
            if (g_use_irq) { cpu::sti_halt(); cpu::cli(); } else cpu::io_wait();
            if (++waits > COMMAND_SPINS) {
                cpu::irq_restore(irq_flags);
                serial::writeln("[nvme] I/O batch timed out");
                return false;
            }
            continue;
        }
        u16 cid = g_io.cq[g_io.cq_head].cid;
        u16 status = g_io.cq[g_io.cq_head].status;
        u32 slot = static_cast<u32>(cid) - 1;
        if (slot < MAX_INFLIGHT && !g_slot_done[slot]) {
            g_slot_done[slot] = true;
            g_slot_status[slot] = static_cast<u16>(status >> 1);
            --pending;
        }
        g_io.cq_head = static_cast<u16>((g_io.cq_head + 1) % g_io.depth);
        if (g_io.cq_head == 0) g_io.phase = static_cast<u16>(g_io.phase ^ 1u);
    }
    __sync_synchronize();
    *g_io.cq_doorbell = g_io.cq_head;
    cpu::irq_restore(irq_flags);
    return true;
}

static bool run_io_batch(u64 start_lba, u32 n, bool write) {
    for (u32 i = 0; i < n; ++i) submit_io(i, start_lba + i, write);
    ring_io_sq_doorbell();
    if (!drain_io(n)) return false;
    bool ok = true;
    for (u32 i = 0; i < n; ++i) ok = ok && (g_slot_status[i] == 0);
    return ok;
}

static bool rmw_transfer(u64 lba, bool write) {
    Sqe cmd{};
    cmd.cdw0 = write ? IO_WRITE : IO_READ;
    cmd.nsid = g_nsid;
    cmd.prp1 = virt_to_phys(g_admin_bounce);
    cmd.cdw10 = static_cast<u32>(lba);
    cmd.cdw11 = static_cast<u32>(lba >> 32);
    return execute(g_io, cmd);
}

static void setup_queue(Queue& q, u32 qid, void* sq, void* cq, u16 depth) {
    q.sq = static_cast<Sqe*>(sq);
    q.cq = static_cast<volatile Cqe*>(cq);
    q.sq_doorbell = reinterpret_cast<volatile u32*>(g_regs + REG_DOORBELL + (2 * qid) * g_stride);
    q.cq_doorbell = reinterpret_cast<volatile u32*>(g_regs + REG_DOORBELL + (2 * qid + 1) * g_stride);
    q.depth = depth;
    q.sq_tail = 0;
    q.cq_head = 0;
    q.phase = 1;
}

static bool enable_msix(u8 cap) {
    u16 ctrl = pci::read16(g_addr, static_cast<u8>(cap + 2));
    u32 table_reg = pci::read32(g_addr, static_cast<u8>(cap + 4));
    u32 bir = table_reg & 0x7u;
    u32 table_off = table_reg & ~0x7u;
    u64 table_bar = pci::bar_address(g_addr, static_cast<int>(bir));
    if (table_bar == 0) return false;
    map_region(table_bar);

    ctrl = static_cast<u16>(ctrl | (1u << 15) | (1u << 14));
    pci::write16(g_addr, static_cast<u8>(cap + 2), ctrl);

    auto* entry = reinterpret_cast<volatile u32*>(g_hhdm + table_bar + table_off);
    entry[3] = 1;
    entry[0] = 0xFEE00000u | (apic::id() << 12);
    entry[1] = 0;
    entry[2] = idt::VEC_NVME;
    entry[3] = 0;

    idt::set_handler(idt::VEC_NVME, &completion_isr);
    ctrl = static_cast<u16>(ctrl & ~(1u << 14));
    pci::write16(g_addr, static_cast<u8>(cap + 2), ctrl);
    g_mode = IrqMode::MsiX;
    return true;
}

static void enable_msi(u8 cap) {
    u16 ctrl = pci::read16(g_addr, static_cast<u8>(cap + 2));
    bool addr64 = (ctrl & (1u << 7)) != 0;
    pci::write32(g_addr, static_cast<u8>(cap + 4), 0xFEE00000u | (apic::id() << 12));
    u8 data_off = static_cast<u8>(cap + 8);
    if (addr64) {
        pci::write32(g_addr, static_cast<u8>(cap + 8), 0);
        data_off = static_cast<u8>(cap + 12);
    }
    pci::write16(g_addr, data_off, idt::VEC_NVME);
    idt::set_handler(idt::VEC_NVME, &completion_isr);
    ctrl = static_cast<u16>((ctrl & ~(0x7u << 4)) | 1u);
    pci::write16(g_addr, static_cast<u8>(cap + 2), ctrl);
    g_mode = IrqMode::Msi;
}

static void drain_interrupts() {
    u64 flags = cpu::irq_save();
    cpu::sti();
    for (u64 i = 0; i < 200000; ++i) asm volatile("pause");
    cpu::cli();
    cpu::irq_restore(flags);
}

static bool probe_interrupt() {
    u32 before = irq_count();
    if (!identify(CNS_CONTROLLER, 0)) return false;
    u64 flags = cpu::irq_save();
    cpu::sti();
    for (u64 i = 0; i < 2000000 && irq_count() == before; ++i) asm volatile("pause");
    cpu::cli();
    cpu::irq_restore(flags);
    return irq_count() != before;
}

static bool fail(const char* what) {
    serial::printf("[nvme] %s failed (status %x)\n", what, static_cast<u32>(g_last_status));
    return false;
}

static void log_controller(u32 vs) {
    char model[41];
    for (int i = 0; i < 40; ++i) {
        u8 c = g_ident[24 + i];
        model[i] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : ' ';
    }
    model[40] = 0;
    int end = 39;
    while (end >= 0 && model[end] == ' ') model[end--] = 0;
    serial::printf("[nvme] controller \"%s\", NVMe %u.%u, queue depth %u\n",
                    model, vs >> 16, (vs >> 8) & 0xFF, static_cast<u32>(g_depth));
}

struct HalDevice : blockdev::Device {
    bool read_sector(u64 sector, void* buf512) override;
    bool write_sector(u64 sector, const void* buf512) override;
    bool read_sectors(u64 start_sector, u64 count, void* buf) override;
    bool write_sectors(u64 start_sector, u64 count, const void* buf) override;
    u64 capacity_sectors() override { return nvme::capacity_sectors(); }
    const char* name() override { return "nvme"; }
};
static HalDevice g_hal;

bool init(u64 hhdm_offset) {
    g_hhdm = hhdm_offset;
    g_found_flag = false;
    pci::scan(&scan_cb, nullptr);
    if (!g_found_flag) {
        serial::writeln("[nvme] no NVMe controller found on the PCI bus");
        return false;
    }
    g_addr = g_found.addr;
    serial::printf("[nvme] found at pci %u:%u.%u\n", g_addr.bus, g_addr.device, g_addr.function);

    u16 command = pci::read16(g_addr, 0x04);
    pci::write16(g_addr, 0x04, static_cast<u16>(command | 0x0406));

    if (pci::read32(g_addr, 0x10) & 1u) {
        serial::writeln("[nvme] BAR0 is not a memory BAR");
        return false;
    }
    u64 bar_phys = pci::bar_address(g_addr, 0);
    if (bar_phys == 0) {
        serial::writeln("[nvme] BAR0 is unassigned");
        return false;
    }
    map_region(bar_phys);
    g_regs = reinterpret_cast<volatile u8*>(g_hhdm + bar_phys);

    u32 vs = reg32(REG_VS);
    if (vs == 0xFFFFFFFFu) {
        serial::writeln("[nvme] controller registers read back as all ones");
        return false;
    }

    u64 cap = reg64(REG_CAP);
    u32 mqes = static_cast<u32>(cap & 0xFFFF);
    u32 timeout = static_cast<u32>((cap >> 24) & 0xFF);
    u32 dstrd = static_cast<u32>((cap >> 32) & 0xF);
    u32 css = static_cast<u32>((cap >> 37) & 0xFF);
    u32 mpsmin = static_cast<u32>((cap >> 48) & 0xF);

    if (!(css & 1u)) {
        serial::writeln("[nvme] controller lacks the NVM command set");
        return false;
    }
    if (mpsmin != 0) {
        serial::writeln("[nvme] controller requires pages larger than 4 KiB");
        return false;
    }
    g_stride = 4u << dstrd;
    if (REG_DOORBELL + 3ull * g_stride + 4 > MAP_BYTES) {
        serial::writeln("[nvme] doorbell stride exceeds the mapped register window");
        return false;
    }
    u16 admin_depth = (mqes + 1 < ADMIN_DEPTH) ? static_cast<u16>(mqes + 1) : ADMIN_DEPTH;
    u16 io_depth = (mqes + 1 < MAX_INFLIGHT + 1) ? static_cast<u16>(mqes + 1) : static_cast<u16>(MAX_INFLIGHT + 1);
    g_depth = io_depth;
    if (admin_depth < 2 || io_depth < 2) {
        serial::writeln("[nvme] controller queue size too small");
        return false;
    }

    u64 limit = static_cast<u64>(timeout + 1) * SPINS_PER_HALF_SECOND;
    wreg32(REG_CC, reg32(REG_CC) & ~CC_EN);
    if (!wait_ready(false, limit)) {
        serial::writeln("[nvme] controller did not stop");
        return false;
    }

    void* asq = alloc_pages(universe::PAGE_SIZE);
    void* acq = alloc_pages(universe::PAGE_SIZE);
    void* isq = alloc_pages(universe::PAGE_SIZE);
    void* icq = alloc_pages(universe::PAGE_SIZE);
    g_ident = static_cast<u8*>(alloc_pages(4096));
    g_admin_bounce = static_cast<u8*>(alloc_pages(4096));
    g_io_bounce_pool = static_cast<u8*>(alloc_pages(4096ull * MAX_INFLIGHT));
    for (auto& d : g_slot_done) d = false;
    for (auto& s : g_slot_status) s = 0;

    setup_queue(g_admin, 0, asq, acq, admin_depth);
    setup_queue(g_io, 1, isq, icq, io_depth);

    wreg32(REG_AQA, ((static_cast<u32>(admin_depth) - 1u) << 16) | (static_cast<u32>(admin_depth) - 1u));
    wreg64(REG_ASQ, virt_to_phys(asq));
    wreg64(REG_ACQ, virt_to_phys(acq));
    wreg32(REG_CC, CC_EN | CC_IOSQES | CC_IOCQES);
    if (!wait_ready(true, limit)) {
        serial::printf("[nvme] controller failed to become ready (csts=%x)\n", reg32(REG_CSTS));
        return false;
    }

    u8 msix_cap = 0, msi_cap = 0;
    if (pci::read16(g_addr, 0x06) & (1u << 4)) {
        u8 p = pci::read8(g_addr, 0x34) & 0xFC;
        for (int guard = 0; p && guard < 48; ++guard) {
            u8 id = pci::read8(g_addr, p);
            if (id == CAP_ID_MSIX && !msix_cap) msix_cap = p;
            else if (id == CAP_ID_MSI && !msi_cap) msi_cap = p;
            p = pci::read8(g_addr, static_cast<u8>(p + 1)) & 0xFC;
        }
    }
    bool armed = msix_cap && enable_msix(msix_cap);
    if (!armed && msi_cap) enable_msi(msi_cap);

    u32 cq_flags = (g_mode == IrqMode::Polled) ? 0x1u : 0x3u;
    if (!admin(ADMIN_CREATE_CQ, 0, virt_to_phys(icq), (static_cast<u32>(io_depth - 1u) << 16) | 1u, cq_flags))
        return fail("create I/O completion queue");
    if (!admin(ADMIN_CREATE_SQ, 0, virt_to_phys(isq), (static_cast<u32>(io_depth - 1u) << 16) | 1u, (1u << 16) | 1u))
        return fail("create I/O submission queue");

    if (!identify(CNS_CONTROLLER, 0)) return fail("identify controller");
    log_controller(vs);

    u32 nsid = 1;
    if (identify(CNS_ACTIVE_LIST, 0)) {
        u32 first = rd32(g_ident);
        if (first) nsid = first;
    }
    if (!identify(CNS_NAMESPACE, nsid)) return fail("identify namespace");

    u64 nsze = rd64(g_ident);
    u8 flbas = g_ident[26];
    u32 fmt = (flbas & 0x0Fu) | ((flbas >> 1) & 0x30u);
    u32 lbaf = rd32(g_ident + 128 + 4 * fmt);
    u32 lbads = (lbaf >> 16) & 0xFF;
    u32 metadata = lbaf & 0xFFFF;

    if (nsze == 0) {
        serial::printf("[nvme] namespace %u reports zero capacity\n", nsid);
        return false;
    }
    if (metadata != 0) {
        serial::writeln("[nvme] namespace uses per-LBA metadata, unsupported");
        return false;
    }
    if (lbads < 9 || lbads > 12) {
        serial::printf("[nvme] unsupported LBA data size 2^%u\n", lbads);
        return false;
    }

    g_nsid = nsid;
    g_lba_shift = lbads;
    g_capacity_sectors = (nsze << lbads) >> 9;
    g_io_max_inflight = io_depth > 1 ? static_cast<u32>(io_depth - 1) : 1;
    if (g_io_max_inflight > MAX_INFLIGHT) g_io_max_inflight = MAX_INFLIGHT;
    serial::printf("[nvme] namespace %u: %lu LBAs of %u bytes\n", nsid, nsze, 1u << lbads);

    if (g_mode != IrqMode::Polled) {
        drain_interrupts();
        g_use_irq = probe_interrupt();
    }

    g_present = true;
    serial::printf("[nvme] ready, capacity=%lu sectors, completion mode=%s, %u request(s) in flight\n",
                    g_capacity_sectors, completion_mode(), g_io_max_inflight);
    blockdev::register_device(&g_hal);
    return true;
}

bool HalDevice::read_sectors(u64 start_sector, u64 count, void* buf) {
    DevGuard guard;
    if (!g_present || start_sector + count > g_capacity_sectors) return false;
    u8* dst = static_cast<u8*>(buf);
    if (g_lba_shift != 9) {
        for (u64 i = 0; i < count; ++i) if (!read_sector(start_sector + i, dst + i * 512)) return false;
        return true;
    }
    while (count > 0) {
        u32 batch = static_cast<u32>(count < g_io_max_inflight ? count : g_io_max_inflight);
        if (!run_io_batch(start_sector, batch, false)) return false;
        for (u32 i = 0; i < batch; ++i)
            __builtin_memcpy(dst + static_cast<u64>(i) * 512, io_bounce_for(i), 512);
        dst += static_cast<u64>(batch) * 512;
        start_sector += batch;
        count -= batch;
    }
    return true;
}

bool HalDevice::write_sectors(u64 start_sector, u64 count, const void* buf) {
    DevGuard guard;
    if (!g_present || start_sector + count > g_capacity_sectors) return false;
    const u8* src = static_cast<const u8*>(buf);
    if (g_lba_shift != 9) {
        for (u64 i = 0; i < count; ++i) if (!write_sector(start_sector + i, src + i * 512)) return false;
        return true;
    }
    while (count > 0) {
        u32 batch = static_cast<u32>(count < g_io_max_inflight ? count : g_io_max_inflight);
        for (u32 i = 0; i < batch; ++i)
            __builtin_memcpy(io_bounce_for(i), src + static_cast<u64>(i) * 512, 512);
        if (!run_io_batch(start_sector, batch, true)) return false;
        src += static_cast<u64>(batch) * 512;
        start_sector += batch;
        count -= batch;
    }
    return true;
}

bool HalDevice::read_sector(u64 sector, void* buf512) {
    DevGuard guard;
    if (!g_present || sector >= g_capacity_sectors) return false;
    if (g_lba_shift == 9) return read_sectors(sector, 1, buf512);
    u64 byte = sector << 9;
    u64 lba = byte >> g_lba_shift;
    u64 off = byte & ((1ull << g_lba_shift) - 1);
    if (!rmw_transfer(lba, false)) return false;
    u8* dst = static_cast<u8*>(buf512);
    __builtin_memcpy(dst, g_admin_bounce + off, 512);
    return true;
}

bool HalDevice::write_sector(u64 sector, const void* buf512) {
    DevGuard guard;
    if (!g_present || sector >= g_capacity_sectors) return false;
    if (g_lba_shift == 9) return write_sectors(sector, 1, buf512);
    u64 byte = sector << 9;
    u64 lba = byte >> g_lba_shift;
    u64 off = byte & ((1ull << g_lba_shift) - 1);
    if (!rmw_transfer(lba, false)) return false;
    const u8* src = static_cast<const u8*>(buf512);
    __builtin_memcpy(g_admin_bounce + off, src, 512);
    return rmw_transfer(lba, true);
}

bool read_sector(u64 sector, void* buf512) { return g_hal.read_sector(sector, buf512); }
bool write_sector(u64 sector, const void* buf512) { return g_hal.write_sector(sector, buf512); }

}
