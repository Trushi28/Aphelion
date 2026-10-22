#include <cosmos/xhci.hpp>
#include <cosmos/pci.hpp>
#include <cosmos/vmm.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/timesrc.hpp>
#include <cosmos/orbital.hpp>

namespace xhci {

namespace {

struct Trb {
    u64 param;
    u32 status;
    u32 control;
};
static_assert(sizeof(Trb) == 16, "a TRB is 16 bytes");

constexpr u32 RING_TRBS = 256;
constexpr u32 EVENT_TRBS = 256;
constexpr u32 MAP_BYTES = 0x20000;

constexpr u32 TRB_NORMAL = 1;
constexpr u32 TRB_SETUP = 2;
constexpr u32 TRB_DATA = 3;
constexpr u32 TRB_STATUS = 4;
constexpr u32 TRB_LINK = 6;
constexpr u32 TRB_ENABLE_SLOT = 9;
constexpr u32 TRB_DISABLE_SLOT = 10;
constexpr u32 TRB_ADDRESS_DEVICE = 11;
constexpr u32 TRB_CONFIGURE_ENDPOINT = 12;
constexpr u32 TRB_EVALUATE_CONTEXT = 13;
constexpr u32 TRB_RESET_ENDPOINT = 14;
constexpr u32 TRB_SET_TR_DEQUEUE = 16;
constexpr u32 EVT_TRANSFER = 32;
constexpr u32 EVT_COMMAND = 33;

constexpr u32 CC_SUCCESS = 1;
constexpr u32 CC_STALL = 6;
constexpr u32 CC_SHORT_PACKET = 13;
constexpr u32 CC_CONTEXT_STATE_ERROR = 19;

constexpr u32 CTRL_CYCLE = 1u << 0;
constexpr u32 CTRL_TC = 1u << 1;
constexpr u32 CTRL_ISP = 1u << 2;
constexpr u32 CTRL_IOC = 1u << 5;
constexpr u32 CTRL_IDT = 1u << 6;

constexpr u32 USBCMD_RS = 1u << 0;
constexpr u32 USBCMD_HCRST = 1u << 1;
constexpr u32 USBSTS_HCH = 1u << 0;
constexpr u32 USBSTS_CNR = 1u << 11;

constexpr u32 PORT_CCS = 1u << 0;
constexpr u32 PORT_PED = 1u << 1;
constexpr u32 PORT_PR = 1u << 4;
constexpr u32 PORT_PP = 1u << 9;
constexpr u32 PORT_PRC = 1u << 21;
constexpr u32 PORT_WRC = 1u << 19;
constexpr u32 PORT_WPR = 1u << 31;
constexpr u32 PORT_RO_MASK = (1u << 0) | (1u << 3) | (0xFu << 10) | (1u << 30);
constexpr u32 PORT_RWS_MASK = (0xFu << 5) | (1u << 9) | (0x3u << 14) | (0x7u << 25);
constexpr u32 PORT_CHANGE_MASK = 0xFEu << 16;

struct Controller {
    bool used = false;
    volatile u8* cap = nullptr;
    volatile u8* op = nullptr;
    volatile u8* rt = nullptr;
    volatile u32* doorbells = nullptr;
    u32 hcsparams1 = 0;
    u32 hcsparams2 = 0;
    u32 hccparams1 = 0;
    u32 slots = 0;
    u32 ports = 0;
    u32 context_size = 32;
    u8 usb3_ports[256];
    u8 slot_types[256];
    bool port_done[256];
    dma::Buffer dcbaa;
    dma::Buffer scratch_array;
    dma::Buffer scratch_pages;
    Ring commands;
    dma::Buffer events;
    dma::Buffer erst;
    u32 event_dequeue = 0;
    u8 event_cycle = 1;
};

u64 g_hhdm = 0;
Controller g_ctrl[MAX_CONTROLLERS];
ControllerInfo g_info[MAX_CONTROLLERS];
u32 g_ctrl_count = 0;
Listed g_listed[MAX_LISTED];
u32 g_listed_count = 0;
Device g_storage[MAX_STORAGE];
u32 g_storage_count = 0;
orbital::Mutex g_io;

u64 now_ms() {
    u64 khz = timesrc::tsc_khz();
    if (khz == 0) khz = 2000000;
    return cpu::rdtsc() / khz;
}

void pause_ms(u32 ms) {
    u64 end = now_ms() + ms;
    while (now_ms() < end) asm volatile("pause" ::: "memory");
}

void barrier() { asm volatile("" ::: "memory"); }

u32 rd32(volatile u8* base, u32 off) { return *reinterpret_cast<volatile u32*>(base + off); }
void wr32(volatile u8* base, u32 off, u32 v) { *reinterpret_cast<volatile u32*>(base + off) = v; }
void wr64(volatile u8* base, u32 off, u64 v) {
    wr32(base, off, static_cast<u32>(v));
    wr32(base, off + 4, static_cast<u32>(v >> 32));
}

bool wait_until(volatile u8* base, u32 off, u32 mask, u32 value, u32 timeout_ms) {
    u64 end = now_ms() + timeout_ms;
    for (;;) {
        if ((rd32(base, off) & mask) == value) return true;
        if (now_ms() >= end) return false;
        asm volatile("pause" ::: "memory");
    }
}

bool map_region(u64 phys_base, u32 bytes) {
    for (u64 off = 0; off < bytes; off += 0x1000) {
        u64 page = phys_base + off;
        if (!constellation::map_4k(g_hhdm + page, page, constellation::WRITABLE | constellation::NO_CACHE | constellation::NO_EXECUTE))
            return false;
    }
    return true;
}

Trb* ring_trbs(Ring& r) { return static_cast<Trb*>(r.mem.virt); }

bool ring_init(Ring& r) {
    r.mem = dma::alloc(RING_TRBS * sizeof(Trb));
    if (!r.mem.ok()) return false;
    r.enqueue = 0;
    r.cycle = 1;
    Trb* t = ring_trbs(r);
    t[RING_TRBS - 1].param = r.mem.phys;
    t[RING_TRBS - 1].status = 0;
    t[RING_TRBS - 1].control = (TRB_LINK << 10) | CTRL_TC;
    return true;
}

void ring_free(Ring& r) {
    if (r.mem.ok()) dma::release(r.mem);
    r.mem = dma::Buffer{};
}

u64 ring_push(Ring& r, u64 param, u32 status, u32 control) {
    Trb* t = ring_trbs(r);
    Trb& slot = t[r.enqueue];
    slot.param = param;
    slot.status = status;
    barrier();
    slot.control = control | (r.cycle ? CTRL_CYCLE : 0);
    barrier();
    u64 phys = r.mem.phys + static_cast<u64>(r.enqueue) * sizeof(Trb);
    ++r.enqueue;
    if (r.enqueue == RING_TRBS - 1) {
        Trb& link = t[RING_TRBS - 1];
        link.control = (link.control & ~CTRL_CYCLE) | (r.cycle ? CTRL_CYCLE : 0);
        barrier();
        r.enqueue = 0;
        r.cycle ^= 1;
    }
    return phys;
}

bool poll_event(Controller& c, Trb& out) {
    Trb* ev = static_cast<Trb*>(c.events.virt);
    const volatile Trb* t = &ev[c.event_dequeue];
    u32 control = t->control;
    if (((control & CTRL_CYCLE) != 0) != (c.event_cycle != 0)) return false;
    barrier();
    out.param = t->param;
    out.status = t->status;
    out.control = control;
    ++c.event_dequeue;
    if (c.event_dequeue == EVENT_TRBS) {
        c.event_dequeue = 0;
        c.event_cycle ^= 1;
    }
    u64 erdp = c.events.phys + static_cast<u64>(c.event_dequeue) * sizeof(Trb);
    wr64(c.rt, 0x20 + 0x18, erdp | (1u << 3));
    return true;
}

bool command(Controller& c, u64 param, u32 status, u32 control, u32* code, u32* slot_out, u32 timeout_ms = 2000) {
    u64 phys = ring_push(c.commands, param, status, control);
    barrier();
    c.doorbells[0] = 0;
    u64 end = now_ms() + timeout_ms;
    Trb ev;
    for (;;) {
        if (poll_event(c, ev)) {
            u32 type = (ev.control >> 10) & 0x3F;
            if (type == EVT_COMMAND && ev.param == phys) {
                if (code) *code = ev.status >> 24;
                if (slot_out) *slot_out = ev.control >> 24;
                return (ev.status >> 24) == CC_SUCCESS;
            }
            continue;
        }
        if (now_ms() >= end) {
            if (code) *code = 0xFFFF;
            return false;
        }
        asm volatile("pause" ::: "memory");
    }
}

struct TransferWait {
    u32 code;
    u32 residual;
    bool timed_out;
};

TransferWait wait_transfer(Controller& c, u32 slot, u32 dci, u64 final_phys, u64 data_phys, u32 timeout_ms) {
    TransferWait w{0, 0, false};
    u64 end = now_ms() + timeout_ms;
    Trb ev;
    for (;;) {
        if (poll_event(c, ev)) {
            u32 type = (ev.control >> 10) & 0x3F;
            if (type != EVT_TRANSFER) continue;
            if ((ev.control >> 24) != slot || ((ev.control >> 16) & 0x1F) != dci) continue;
            u32 code = ev.status >> 24;
            if (ev.param == final_phys) {
                w.code = code;
                if (data_phys == final_phys) w.residual = ev.status & 0xFFFFFF;
                return w;
            }
            if (ev.param == data_phys) {
                w.residual = ev.status & 0xFFFFFF;
                if (code != CC_SUCCESS && code != CC_SHORT_PACKET) {
                    w.code = code;
                    return w;
                }
            }
            continue;
        }
        if (now_ms() >= end) {
            w.timed_out = true;
            return w;
        }
        asm volatile("pause" ::: "memory");
    }
}

Result map_code(const TransferWait& w) {
    if (w.timed_out) return Result::Timeout;
    if (w.code == CC_SUCCESS || w.code == CC_SHORT_PACKET) return Result::Ok;
    if (w.code == CC_STALL) return Result::Stall;
    return Result::Error;
}

u32* ctx_dword(Controller& c, void* base, u32 index) {
    return reinterpret_cast<u32*>(static_cast<u8*>(base) + static_cast<u64>(index) * c.context_size);
}

bool reset_endpoint(Controller& c, Device& d, Ring& ring, u32 dci) {
    u32 code = 0;
    bool ok = command(c, 0, 0, (TRB_RESET_ENDPOINT << 10) | (static_cast<u32>(d.slot) << 24) | (dci << 16), &code, nullptr);
    if (!ok && code != CC_CONTEXT_STATE_ERROR) return false;
    u64 dq = ring.mem.phys + static_cast<u64>(ring.enqueue) * sizeof(Trb);
    ok = command(c, dq | (ring.cycle ? 1 : 0), 0, (TRB_SET_TR_DEQUEUE << 10) | (static_cast<u32>(d.slot) << 24) | (dci << 16), &code, nullptr);
    return ok || code == CC_CONTEXT_STATE_ERROR;
}

Result control_locked(Controller& c, Device& d, const usbproto::Setup& s, u32 length, bool in, u32* actual, u32 timeout_ms) {
    u64 setup_word = 0;
    __builtin_memcpy(&setup_word, &s, sizeof(setup_word));
    u32 trt = length == 0 ? 0 : (in ? 3 : 2);
    ring_push(d.ep0, setup_word, 8, (TRB_SETUP << 10) | CTRL_IDT | (trt << 16));
    u64 data_phys = 0;
    if (length) data_phys = ring_push(d.ep0, d.small.phys, length, (TRB_DATA << 10) | (in ? (1u << 16) : 0) | CTRL_ISP);
    bool status_in = !(in && length > 0);
    u64 status_phys = ring_push(d.ep0, 0, 0, (TRB_STATUS << 10) | (status_in ? (1u << 16) : 0) | CTRL_IOC);
    barrier();
    c.doorbells[d.slot] = 1;
    TransferWait w = wait_transfer(c, d.slot, 1, status_phys, data_phys ? data_phys : status_phys, timeout_ms);
    Result r = map_code(w);
    if (r == Result::Timeout) d.dead = true;
    if ((r == Result::Stall || r == Result::Error) && !reset_endpoint(c, d, d.ep0, 1)) d.dead = true;
    if (r == Result::Ok && actual) *actual = length > w.residual ? length - w.residual : 0;
    return r;
}

Result bulk_locked(Controller& c, Device& d, bool in, u32 length, u32* actual, u32 timeout_ms) {
    Ring& ring = in ? d.bulk_in : d.bulk_out;
    u32 dci = in ? d.in_dci : d.out_dci;
    u64 phys = ring_push(ring, d.bounce.phys, length, (TRB_NORMAL << 10) | CTRL_IOC | CTRL_ISP);
    barrier();
    c.doorbells[d.slot] = dci;
    TransferWait w = wait_transfer(c, d.slot, dci, phys, phys, timeout_ms);
    Result r = map_code(w);
    if (r == Result::Timeout) d.dead = true;
    if (r == Result::Ok && actual) *actual = length > w.residual ? length - w.residual : 0;
    return r;
}

Controller& ctrl_of(Device& d) { return g_ctrl[d.controller]; }

bool ctl_request(Controller& c, Device& d, u8 type, u8 request, u16 value, u16 index, u16 length, u32* actual = nullptr) {
    usbproto::Setup s = usbproto::setup_packet(type, request, value, index, length);
    Result r = control_locked(c, d, s, length, (type & 0x80) != 0, actual, 1500);
    return r == Result::Ok;
}

u32 ep0_mps(Speed s) {
    switch (s) {
        case Speed::Low: return 8;
        case Speed::Full: return 64;
        case Speed::High: return 64;
        default: return 512;
    }
}

void free_device(Controller& c, Device& d) {
    ring_free(d.ep0);
    ring_free(d.bulk_in);
    ring_free(d.bulk_out);
    if (d.out_context.ok()) dma::release(d.out_context);
    if (d.in_context.ok()) dma::release(d.in_context);
    if (d.small.ok()) dma::release(d.small);
    if (d.bounce.ok()) dma::release(d.bounce);
    d.out_context = d.in_context = d.small = d.bounce = dma::Buffer{};
    (void)c;
}

void disable_slot(Controller& c, Device& d) {
    if (d.slot) {
        u32 code = 0;
        command(c, 0, 0, (TRB_DISABLE_SLOT << 10) | (static_cast<u32>(d.slot) << 24), &code, nullptr);
        static_cast<u64*>(c.dcbaa.virt)[d.slot] = 0;
    }
    free_device(c, d);
}

void list_device(u32 ci, u32 port, Speed speed, u16 vid, u16 pid, u8 cls, bool storage, const char* note) {
    if (g_listed_count == MAX_LISTED) return;
    g_listed[g_listed_count++] = Listed{static_cast<u8>(ci), static_cast<u8>(port), speed, vid, pid, cls, storage, note};
}

bool enumerate(Controller& c, u32 ci, u32 port, Speed speed, Device& d) {
    d = Device{};
    d.controller = static_cast<u8>(ci);
    d.port = static_cast<u8>(port);
    d.speed = speed;

    u32 code = 0, slot = 0;
    if (!command(c, 0, 0, (TRB_ENABLE_SLOT << 10) | (static_cast<u32>(c.slot_types[port]) << 16), &code, &slot) || slot == 0 || slot > c.slots) {
        serial::printf("[xhci] port %u: enable slot failed (code %u)\n", port, code);
        list_device(ci, port, speed, 0, 0, 0, false, "no device slot");
        return false;
    }
    d.slot = static_cast<u8>(slot);
    d.in_use = true;

    d.out_context = dma::alloc(4096);
    d.in_context = dma::alloc(4096);
    d.small = dma::alloc(SMALL_BYTES);
    if (!d.out_context.ok() || !d.in_context.ok() || !d.small.ok() || !ring_init(d.ep0)) {
        serial::writeln("[xhci] out of DMA memory while enumerating");
        list_device(ci, port, speed, 0, 0, 0, false, "out of memory");
        disable_slot(c, d);
        return false;
    }

    u32* icc = ctx_dword(c, d.in_context.virt, 0);
    icc[0] = 0;
    icc[1] = 0x3;
    u32* sctx = ctx_dword(c, d.in_context.virt, 1);
    sctx[0] = (static_cast<u32>(speed) << 20) | (1u << 27);
    sctx[1] = port << 16;
    u32* ep0 = ctx_dword(c, d.in_context.virt, 2);
    u32 mps = ep0_mps(speed);
    ep0[1] = (3u << 1) | (4u << 3) | (mps << 16);
    u64 deq = d.ep0.mem.phys | 1;
    ep0[2] = static_cast<u32>(deq);
    ep0[3] = static_cast<u32>(deq >> 32);
    ep0[4] = 8;

    static_cast<u64*>(c.dcbaa.virt)[slot] = d.out_context.phys;
    if (!command(c, d.in_context.phys, 0, (TRB_ADDRESS_DEVICE << 10) | (slot << 24), &code, nullptr, 4000)) {
        serial::printf("[xhci] port %u: address device failed (code %u)\n", port, code);
        list_device(ci, port, speed, 0, 0, 0, false, "did not accept an address");
        disable_slot(c, d);
        return false;
    }

    u32 got = 0;
    if (!ctl_request(c, d, 0x80, usbproto::REQ_GET_DESCRIPTOR, usbproto::DESC_DEVICE << 8, 0, 8, &got) || got < 8) {
        list_device(ci, port, speed, 0, 0, 0, false, "no device descriptor");
        disable_slot(c, d);
        return false;
    }
    const u8* dd = static_cast<const u8*>(d.small.virt);
    u32 real_mps = dd[7];
    if (speed >= Speed::Super) real_mps = real_mps >= 9 ? (1u << real_mps) : 512;
    if (real_mps != mps && real_mps >= 8) {
        u32* ictl = ctx_dword(c, d.in_context.virt, 0);
        ictl[0] = 0;
        ictl[1] = 0x2;
        u32* e0 = ctx_dword(c, d.in_context.virt, 2);
        e0[1] = (e0[1] & 0xFFFFu) | (real_mps << 16);
        if (!command(c, d.in_context.phys, 0, (TRB_EVALUATE_CONTEXT << 10) | (slot << 24), &code, nullptr)) {
            serial::printf("[xhci] port %u: evaluate context failed (code %u)\n", port, code);
            list_device(ci, port, speed, 0, 0, 0, false, "could not set the control packet size");
            disable_slot(c, d);
            return false;
        }
    }

    if (!ctl_request(c, d, 0x80, usbproto::REQ_GET_DESCRIPTOR, usbproto::DESC_DEVICE << 8, 0, 18, &got) || got < 18) {
        list_device(ci, port, speed, 0, 0, 0, false, "no device descriptor");
        disable_slot(c, d);
        return false;
    }
    dd = static_cast<const u8*>(d.small.virt);
    d.device_class = dd[4];
    d.vendor = static_cast<u16>(dd[8] | (dd[9] << 8));
    d.product = static_cast<u16>(dd[10] | (dd[11] << 8));
    u8 num_configs = dd[17];

    if (!ctl_request(c, d, 0x80, usbproto::REQ_GET_DESCRIPTOR, usbproto::DESC_CONFIG << 8, 0, 9, &got) || got < 9) {
        list_device(ci, port, speed, d.vendor, d.product, d.device_class, false, "no configuration descriptor");
        disable_slot(c, d);
        return false;
    }
    u32 total = static_cast<u32>(static_cast<const u8*>(d.small.virt)[2] | (static_cast<const u8*>(d.small.virt)[3] << 8));
    if (total > 1024) total = 1024;
    if (total < 9) total = 9;
    if (!ctl_request(c, d, 0x80, usbproto::REQ_GET_DESCRIPTOR, usbproto::DESC_CONFIG << 8, 0, static_cast<u16>(total), &got) || got < 9) {
        list_device(ci, port, speed, d.vendor, d.product, d.device_class, false, "no configuration descriptor");
        disable_slot(c, d);
        return false;
    }
    d.storage = usbproto::find_mass_storage(static_cast<const u8*>(d.small.virt), got);
    (void)num_configs;
    if (!d.storage.found) {
        const char* note = d.device_class == 9 ? "hub (hubs are not supported)" : (d.device_class == 3 ? "input device (no HID driver yet)" : "not a bulk-only mass-storage device");
        list_device(ci, port, speed, d.vendor, d.product, d.device_class, false, note);
        disable_slot(c, d);
        return false;
    }

    if (!ctl_request(c, d, 0x00, usbproto::REQ_SET_CONFIGURATION, d.storage.configuration, 0, 0)) {
        list_device(ci, port, speed, d.vendor, d.product, d.device_class, false, "set configuration failed");
        disable_slot(c, d);
        return false;
    }

    d.in_dci = static_cast<u8>(d.storage.bulk_in * 2 + 1);
    d.out_dci = static_cast<u8>(d.storage.bulk_out * 2);
    d.bounce = dma::alloc(BOUNCE_BYTES);
    if (!d.bounce.ok() || !ring_init(d.bulk_in) || !ring_init(d.bulk_out)) {
        list_device(ci, port, speed, d.vendor, d.product, d.device_class, false, "out of memory");
        disable_slot(c, d);
        return false;
    }

    u32 max_dci = d.in_dci > d.out_dci ? d.in_dci : d.out_dci;
    u32* ic = ctx_dword(c, d.in_context.virt, 0);
    for (u32 i = 0; i < c.context_size / 4 * 33; ++i) ic[i] = 0;
    ic[1] = 1u | (1u << d.in_dci) | (1u << d.out_dci);
    u32* out_slot = ctx_dword(c, d.out_context.virt, 0);
    u32* in_slot = ctx_dword(c, d.in_context.virt, 1);
    for (u32 i = 0; i < 4; ++i) in_slot[i] = out_slot[i];
    in_slot[0] = (in_slot[0] & 0x07FFFFFFu) | (max_dci << 27);

    auto fill_ep = [&](u32 dci, u32 type, u32 packet, u32 burst, Ring& ring) {
        u32* e = ctx_dword(c, d.in_context.virt, dci + 1);
        e[0] = 0;
        e[1] = (3u << 1) | (type << 3) | (burst << 8) | (packet << 16);
        u64 dq = ring.mem.phys | 1;
        e[2] = static_cast<u32>(dq);
        e[3] = static_cast<u32>(dq >> 32);
        e[4] = packet;
    };
    fill_ep(d.in_dci, 6, d.storage.in_max_packet, d.storage.in_burst, d.bulk_in);
    fill_ep(d.out_dci, 2, d.storage.out_max_packet, d.storage.out_burst, d.bulk_out);

    if (!command(c, d.in_context.phys, 0, (TRB_CONFIGURE_ENDPOINT << 10) | (slot << 24), &code, nullptr, 4000)) {
        serial::printf("[xhci] port %u: configure endpoint failed (code %u)\n", port, code);
        list_device(ci, port, speed, d.vendor, d.product, d.device_class, false, "endpoints could not be configured");
        disable_slot(c, d);
        return false;
    }
    list_device(ci, port, speed, d.vendor, d.product, d.device_class, true, "bulk-only mass storage");
    return true;
}

Speed port_speed(u32 portsc) {
    u32 s = (portsc >> 10) & 0xF;
    if (s >= 1 && s <= 5) return static_cast<Speed>(s);
    return Speed::Unknown;
}

u32 portsc_neutral(u32 v) { return v & (PORT_RO_MASK | PORT_RWS_MASK); }

bool reset_port(Controller& c, u32 port) {
    u32 off = 0x400 + (port - 1) * 0x10;
    u32 v = rd32(c.op, off);
    bool usb3 = c.usb3_ports[port] != 0;
    u32 pls = (v >> 5) & 0xF;
    u32 request = (usb3 && (pls == 6 || pls == 10)) ? PORT_WPR : PORT_PR;
    wr32(c.op, off, portsc_neutral(v) | request);
    u32 done_bit = request == PORT_WPR ? PORT_WRC : PORT_PRC;
    u64 end = now_ms() + 800;
    for (;;) {
        v = rd32(c.op, off);
        if (v & done_bit) break;
        if (now_ms() >= end) return false;
        asm volatile("pause" ::: "memory");
    }
    wr32(c.op, off, portsc_neutral(v) | (v & PORT_CHANGE_MASK));
    pause_ms(usb3 ? 20 : 20);
    v = rd32(c.op, off);
    return (v & PORT_CCS) && (v & PORT_PED);
}

bool usb3_trained(Controller& c, u32 off, u32* pls_out) {
    u64 end = now_ms() + 400;
    for (;;) {
        u32 v = rd32(c.op, off);
        u32 pls = (v >> 5) & 0xF;
        *pls_out = pls;
        if (!(v & PORT_CCS)) return false;
        if ((v & PORT_PED) && pls == 0) return true;
        if (pls == 6 || pls == 10) return false;
        if (now_ms() >= end) return false;
        asm volatile("pause" ::: "memory");
    }
}

void scan_ports(Controller& c, u32 ci, bool last) {
    for (u32 port = 1; port <= c.ports; ++port) {
        if (c.port_done[port]) continue;
        u32 off = 0x400 + (port - 1) * 0x10;
        u32 v = rd32(c.op, off);
        if (!(v & PORT_PP)) {
            wr32(c.op, off, portsc_neutral(v) | PORT_PP);
            continue;
        }
        if (!(v & PORT_CCS)) continue;
        bool usb3 = c.usb3_ports[port] != 0;
        bool enabled = false;
        if (usb3) {
            u32 pls = 0;
            enabled = usb3_trained(c, off, &pls);
            if (!enabled && pls != 6 && pls != 10) {
                if (!last) continue;
                c.port_done[port] = true;
                serial::printf("[xhci] port %u: a SuperSpeed device is connected but its link did not train\n", port);
                list_device(ci, port, Speed::Unknown, 0, 0, 0, false, "SuperSpeed link did not train");
                continue;
            }
        }
        c.port_done[port] = true;
        if (!enabled && !reset_port(c, port)) {
            serial::printf("[xhci] port %u: a device is connected but the port would not enable\n", port);
            list_device(ci, port, Speed::Unknown, 0, 0, 0, false, "port would not enable");
            continue;
        }
        v = rd32(c.op, off);
        wr32(c.op, off, portsc_neutral(v) | (v & PORT_CHANGE_MASK));
        Speed speed = port_speed(v);
        if (speed == Speed::Unknown) {
            list_device(ci, port, speed, 0, 0, 0, false, "unknown port speed");
            continue;
        }
        Device tmp;
        if (enumerate(c, ci, port, speed, tmp)) {
            if (g_storage_count < MAX_STORAGE) g_storage[g_storage_count++] = tmp;
            else disable_slot(c, tmp);
        }
    }
}

bool take_ownership(Controller& c, bool* took) {
    u32 xecp = (c.hccparams1 >> 16) & 0xFFFF;
    u32 off = xecp * 4;
    for (int guard = 0; guard < 64 && xecp && off + 8 <= MAP_BYTES; ++guard) {
        u32 hdr = rd32(c.cap, off);
        u32 id = hdr & 0xFF;
        if (id == 1) {
            u32 legacy = rd32(c.cap, off);
            if (legacy & (1u << 16)) {
                wr32(c.cap, off, legacy | (1u << 24));
                if (!wait_until(c.cap, off, 1u << 16, 0, 1500)) wr32(c.cap, off, rd32(c.cap, off) & ~(1u << 16));
                *took = true;
            }
            u32 ctl = rd32(c.cap, off + 4);
            ctl &= ~((1u << 0) | (1u << 4) | (0x7u << 13));
            ctl |= 0x7u << 29;
            wr32(c.cap, off + 4, ctl);
        } else if (id == 2 && off + 16 <= MAP_BYTES) {
            u32 major = (hdr >> 24) & 0xFF;
            u32 d2 = rd32(c.cap, off + 8);
            u32 first = d2 & 0xFF, count = (d2 >> 8) & 0xFF;
            u32 d3 = rd32(c.cap, off + 12);
            for (u32 p = first; p < first + count && p < 256; ++p) {
                c.usb3_ports[p] = major >= 3 ? 1 : 0;
                c.slot_types[p] = static_cast<u8>(d3 & 0x1F);
            }
        }
        u32 next = (hdr >> 8) & 0xFF;
        if (!next) break;
        off += next * 4;
    }
    return true;
}

void free_controller(Controller& c) {
    ring_free(c.commands);
    if (c.events.ok()) dma::release(c.events);
    if (c.erst.ok()) dma::release(c.erst);
    if (c.dcbaa.ok()) dma::release(c.dcbaa);
    if (c.scratch_array.ok()) dma::release(c.scratch_array);
    if (c.scratch_pages.ok()) dma::release(c.scratch_pages);
    c.events = c.erst = c.dcbaa = c.scratch_array = c.scratch_pages = dma::Buffer{};
}

bool start(const pci::Device& dev, u32 index) {
    Controller& c = g_ctrl[index];
    c = Controller{};
    ControllerInfo& info = g_info[index];
    info = ControllerInfo{};
    info.vendor = dev.vendor_id;
    info.device = dev.device_id;
    info.bus = dev.addr.bus;
    info.slot = dev.addr.device;
    info.function = dev.addr.function;
    info.status = "not started";

    u16 command_reg = pci::read16(dev.addr, 0x04);
    pci::write16(dev.addr, 0x04, static_cast<u16>(command_reg | 0x0406));
    u64 bar = pci::bar_address(dev.addr, 0);
    if (bar == 0 || !map_region(bar, MAP_BYTES)) {
        info.status = "BAR 0 could not be mapped";
        return false;
    }
    c.cap = reinterpret_cast<volatile u8*>(g_hhdm + bar);
    u32 caplength = *c.cap;
    c.hcsparams1 = rd32(c.cap, 0x04);
    c.hcsparams2 = rd32(c.cap, 0x08);
    c.hccparams1 = rd32(c.cap, 0x10);
    u32 dboff = rd32(c.cap, 0x14) & ~3u;
    u32 rtsoff = rd32(c.cap, 0x18) & ~0x1Fu;
    if (caplength < 0x20 || caplength > 0x100 || dboff + 0x400 > MAP_BYTES || rtsoff + 0x40 > MAP_BYTES || rd32(c.cap, 0) == 0xFFFFFFFFu) {
        info.status = "implausible capability registers";
        return false;
    }
    c.op = c.cap + caplength;
    c.rt = c.cap + rtsoff;
    c.doorbells = reinterpret_cast<volatile u32*>(c.cap + dboff);
    c.slots = c.hcsparams1 & 0xFF;
    c.ports = (c.hcsparams1 >> 24) & 0xFF;
    c.context_size = (c.hccparams1 & (1u << 2)) ? 64 : 32;
    info.ports = c.ports;
    info.slots = c.slots;
    info.context64 = c.context_size == 64;
    if (c.slots == 0 || c.ports == 0 || 0x400 + c.ports * 0x10 > MAP_BYTES) {
        info.status = "implausible port or slot count";
        return false;
    }
    if (!(rd32(c.op, 0x08) & 1)) {
        info.status = "controller does not support 4 KiB pages";
        return false;
    }

    bool took = false;
    take_ownership(c, &took);
    info.took_over_from_firmware = took;

    u32 cmd = rd32(c.op, 0x00);
    if (rd32(c.op, 0x04) & USBSTS_HCH) cmd &= ~USBCMD_RS;
    else {
        wr32(c.op, 0x00, cmd & ~USBCMD_RS);
        if (!wait_until(c.op, 0x04, USBSTS_HCH, USBSTS_HCH, 500)) {
            info.status = "controller would not halt";
            return false;
        }
    }
    wr32(c.op, 0x00, USBCMD_HCRST);
    if (!wait_until(c.op, 0x00, USBCMD_HCRST, 0, 1500) || !wait_until(c.op, 0x04, USBSTS_CNR, 0, 1500)) {
        info.status = "controller reset timed out";
        return false;
    }

    if (c.slots > 64) c.slots = 64;
    wr32(c.op, 0x38, c.slots);

    c.dcbaa = dma::alloc((c.slots + 1) * 8);
    c.events = dma::alloc(EVENT_TRBS * sizeof(Trb));
    c.erst = dma::alloc(64);
    if (!c.dcbaa.ok() || !c.events.ok() || !c.erst.ok() || !ring_init(c.commands)) {
        info.status = "out of DMA memory";
        free_controller(c);
        return false;
    }

    u32 scratch = ((c.hcsparams2 >> 21) & 0x1F) << 5 | ((c.hcsparams2 >> 27) & 0x1F);
    if (scratch) {
        c.scratch_array = dma::alloc(scratch * 8);
        c.scratch_pages = dma::alloc(static_cast<u64>(scratch) * 4096);
        if (!c.scratch_array.ok() || !c.scratch_pages.ok()) {
            info.status = "out of DMA memory for the scratchpad";
            free_controller(c);
            return false;
        }
        for (u32 i = 0; i < scratch; ++i) static_cast<u64*>(c.scratch_array.virt)[i] = c.scratch_pages.phys + static_cast<u64>(i) * 4096;
        static_cast<u64*>(c.dcbaa.virt)[0] = c.scratch_array.phys;
    }
    wr64(c.op, 0x30, c.dcbaa.phys);
    wr64(c.op, 0x18, c.commands.mem.phys | 1);

    u64* seg = static_cast<u64*>(c.erst.virt);
    seg[0] = c.events.phys;
    seg[1] = EVENT_TRBS;
    wr32(c.rt, 0x20 + 0x08, 1);
    wr64(c.rt, 0x20 + 0x18, c.events.phys);
    wr64(c.rt, 0x20 + 0x10, c.erst.phys);
    wr32(c.rt, 0x20 + 0x00, 0);
    c.event_dequeue = 0;
    c.event_cycle = 1;

    wr32(c.op, 0x00, USBCMD_RS);
    if (!wait_until(c.op, 0x04, USBSTS_HCH, 0, 500)) {
        info.status = "controller would not run";
        free_controller(c);
        return false;
    }
    c.used = true;
    info.started = true;
    info.status = "running";

    for (u32 port = 1; port <= c.ports; ++port) {
        u32 off = 0x400 + (port - 1) * 0x10;
        u32 v = rd32(c.op, off);
        if (!(v & PORT_PP)) wr32(c.op, off, portsc_neutral(v) | PORT_PP);
    }
    pause_ms(300);
    scan_ports(c, index, false);
    pause_ms(700);
    scan_ports(c, index, true);
    return true;
}

struct ScanCtx {
    u32 found = 0;
    pci::Device devices[MAX_CONTROLLERS];
};

void scan_cb(const pci::Device& d, void* ctx) {
    ScanCtx* s = static_cast<ScanCtx*>(ctx);
    if (d.class_code == 0x0C && d.subclass == 0x03 && d.prog_if == 0x30 && s->found < MAX_CONTROLLERS) s->devices[s->found++] = d;
}

}

bool init(u64 hhdm_offset) {
    g_hhdm = hhdm_offset;
    g_ctrl_count = 0;
    g_listed_count = 0;
    g_storage_count = 0;
    ScanCtx scan;
    pci::scan(&scan_cb, &scan);
    for (u32 i = 0; i < scan.found; ++i) {
        g_io.lock();
        bool ok = start(scan.devices[i], g_ctrl_count);
        g_io.unlock();
        const ControllerInfo& info = g_info[g_ctrl_count];
        serial::printf("[xhci] %x:%x at %u:%u.%u: %s%s, %u port(s), %u slot(s), %u-byte contexts\n", static_cast<unsigned>(info.vendor),
                        static_cast<unsigned>(info.device), static_cast<unsigned>(info.bus), static_cast<unsigned>(info.slot),
                        static_cast<unsigned>(info.function), info.status, info.took_over_from_firmware ? " (taken over from the firmware)" : "",
                        info.ports, info.slots, info.context64 ? 64u : 32u);
        ++g_ctrl_count;
        (void)ok;
    }
    return g_ctrl_count > 0;
}

u32 controller_count() { return g_ctrl_count; }
const ControllerInfo& controller(u32 index) { return g_info[index < MAX_CONTROLLERS ? index : 0]; }
u32 listed_count() { return g_listed_count; }
const Listed& listed(u32 index) { return g_listed[index < MAX_LISTED ? index : 0]; }
u32 storage_count() { return g_storage_count; }
Device* storage_device(u32 index) { return index < g_storage_count ? &g_storage[index] : nullptr; }

Result control(Device& dev, const usbproto::Setup& setup, void* data, u32 length, u32* actual, u32 timeout_ms) {
    if (dev.dead || length > SMALL_BYTES) return Result::Error;
    g_io.lock();
    bool in = (setup.request_type & 0x80) != 0;
    if (!in && length) __builtin_memcpy(dev.small.virt, data, length);
    Result r = control_locked(ctrl_of(dev), dev, setup, length, in, actual, timeout_ms);
    if (r == Result::Ok && in && length && data) {
        u32 n = actual ? *actual : length;
        __builtin_memcpy(data, dev.small.virt, n < length ? n : length);
    }
    g_io.unlock();
    return r;
}

Result bulk_out(Device& dev, const void* data, u32 length, u32 timeout_ms) {
    if (dev.dead || length == 0 || length > BOUNCE_BYTES) return Result::Error;
    g_io.lock();
    __builtin_memcpy(dev.bounce.virt, data, length);
    Result r = bulk_locked(ctrl_of(dev), dev, false, length, nullptr, timeout_ms);
    g_io.unlock();
    return r;
}

Result bulk_in(Device& dev, void* data, u32 length, u32* actual, u32 timeout_ms) {
    if (dev.dead || length == 0 || length > BOUNCE_BYTES) return Result::Error;
    g_io.lock();
    u32 got = 0;
    Result r = bulk_locked(ctrl_of(dev), dev, true, length, &got, timeout_ms);
    if (r == Result::Ok) __builtin_memcpy(data, dev.bounce.virt, got < length ? got : length);
    if (actual) *actual = got;
    g_io.unlock();
    return r;
}

bool clear_halt(Device& dev, bool in) {
    if (dev.dead) return false;
    g_io.lock();
    Controller& c = ctrl_of(dev);
    Ring& ring = in ? dev.bulk_in : dev.bulk_out;
    u32 dci = in ? dev.in_dci : dev.out_dci;
    bool ok = reset_endpoint(c, dev, ring, dci);
    if (ok) {
        u8 address = static_cast<u8>((in ? 0x80 : 0x00) | (in ? dev.storage.bulk_in : dev.storage.bulk_out));
        ok = ctl_request(c, dev, 0x02, usbproto::REQ_CLEAR_FEATURE, 0, address, 0);
    }
    g_io.unlock();
    return ok;
}

const char* speed_name(Speed s) {
    switch (s) {
        case Speed::Low: return "low-speed (1.5 Mb/s)";
        case Speed::Full: return "full-speed (12 Mb/s)";
        case Speed::High: return "high-speed (480 Mb/s)";
        case Speed::Super: return "SuperSpeed (5 Gb/s)";
        case Speed::SuperPlus: return "SuperSpeed+ (10 Gb/s or more)";
        default: return "unknown speed";
    }
}

}
