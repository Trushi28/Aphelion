#include <cosmos/usbstor.hpp>
#include <cosmos/usbproto.hpp>
#include <cosmos/blockdev.hpp>
#include <cosmos/orbital.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/timesrc.hpp>
#include <cosmos/cpu.hpp>

namespace usbstor {

namespace {

namespace sc = usbproto::scsi;

constexpr u32 IO_TIMEOUT_MS = 8000;
constexpr u32 CONTROL_TIMEOUT_MS = 2000;
constexpr u32 READY_ATTEMPTS = 14;
constexpr u32 READY_PAUSE_MS = 250;
constexpr u32 COMMAND_ATTEMPTS = 3;
constexpr u32 MAX_REPORTED_ERRORS = 8;
constexpr u8 SENSE_NOT_READY = 2;
constexpr u8 SENSE_ILLEGAL_REQUEST = 5;
constexpr u8 SENSE_UNIT_ATTENTION = 6;
constexpr u8 SENSE_UNKNOWN = 0xFF;

enum class Outcome : u8 { Good, Failed, Broken };

LunInfo g_luns[MAX_LUNS];
u32 g_lun_count;
u32 g_tag;
u32 g_reported_errors;
orbital::Mutex g_cmd;
const LunInfo* g_window_lun;
const char* g_window_status;

u64 now_ms() {
    u64 khz = timesrc::tsc_khz();
    if (khz == 0) khz = 2000000;
    return cpu::rdtsc() / khz;
}

void pause_ms(u32 ms) {
    u64 end = now_ms() + ms;
    while (now_ms() < end) asm volatile("pause" ::: "memory");
}

Outcome broken(xhci::Device& d) {
    usbproto::Setup s = usbproto::setup_packet(0x21, usbproto::REQ_BOT_RESET, 0, d.storage.interface_number, 0);
    bool ok = xhci::control(d, s, nullptr, 0, nullptr, CONTROL_TIMEOUT_MS) == xhci::Result::Ok;
    ok = xhci::clear_halt(d, true) && ok;
    ok = xhci::clear_halt(d, false) && ok;
    if (!ok) d.dead = true;
    return Outcome::Broken;
}

Outcome transact(LunInfo& l, const u8* cdb, u8 cdb_length, bool in, void* data, u32 data_length, u32* got) {
    xhci::Device& d = *l.dev;
    if (d.dead) return Outcome::Broken;
    u32 tag = ++g_tag;
    u8 cbw[usbproto::CBW_BYTES];
    usbproto::build_cbw(cbw, tag, data_length, in, l.lun, cdb, cdb_length);
    if (xhci::bulk_out(d, cbw, usbproto::CBW_BYTES, IO_TIMEOUT_MS) != xhci::Result::Ok) return broken(d);

    u32 moved = 0;
    if (data_length) {
        xhci::Result r;
        if (in) {
            r = xhci::bulk_in(d, data, data_length, &moved, IO_TIMEOUT_MS);
        } else {
            r = xhci::bulk_out(d, data, data_length, IO_TIMEOUT_MS);
            moved = data_length;
        }
        if (r == xhci::Result::Stall) {
            if (!xhci::clear_halt(d, in)) return broken(d);
        } else if (r != xhci::Result::Ok) {
            return broken(d);
        }
    }

    u8 csw[usbproto::CSW_BYTES];
    u32 n = 0;
    xhci::Result r = xhci::bulk_in(d, csw, usbproto::CSW_BYTES, &n, IO_TIMEOUT_MS);
    if (r == xhci::Result::Stall) {
        if (!xhci::clear_halt(d, true)) return broken(d);
        r = xhci::bulk_in(d, csw, usbproto::CSW_BYTES, &n, IO_TIMEOUT_MS);
    }
    if (r != xhci::Result::Ok) return broken(d);

    usbproto::Csw status = usbproto::parse_csw(csw, n, tag);
    if (status.result == usbproto::CswResult::Invalid || status.result == usbproto::CswResult::PhaseError) return broken(d);
    if (status.residue > data_length) return broken(d);
    u32 accounted = data_length - status.residue;
    if (in && moved < accounted) accounted = moved;
    if (got) *got = accounted;
    return status.result == usbproto::CswResult::Passed ? Outcome::Good : Outcome::Failed;
}

sc::Sense fetch_sense(LunInfo& l) {
    u8 cdb[16];
    u8 length = sc::request_sense(cdb, 18);
    u8 buf[18];
    for (u32 i = 0; i < sizeof(buf); ++i) buf[i] = 0;
    u32 n = 0;
    if (transact(l, cdb, length, true, buf, sizeof(buf), &n) != Outcome::Good) return sc::Sense{SENSE_UNKNOWN, 0, 0};
    return sc::parse_sense(buf, n);
}

bool run(LunInfo& l, const u8* cdb, u8 cdb_length, bool in, void* data, u32 data_length, u32* got, sc::Sense* sense_out) {
    g_cmd.lock();
    bool ok = false;
    for (u32 attempt = 0; attempt < COMMAND_ATTEMPTS; ++attempt) {
        Outcome o = transact(l, cdb, cdb_length, in, data, data_length, got);
        if (o == Outcome::Good) { ok = true; break; }
        if (o == Outcome::Broken) break;
        sc::Sense s = fetch_sense(l);
        if (sense_out) *sense_out = s;
        if (s.key == SENSE_UNIT_ATTENTION) continue;
        break;
    }
    g_cmd.unlock();
    return ok;
}

void report_error(const LunInfo& l, const char* what, u64 lba, u32 blocks, const sc::Sense& s) {
    if (g_reported_errors >= MAX_REPORTED_ERRORS) return;
    ++g_reported_errors;
    serial::printf("[usb-storage] %s failed at sector %lu (%u block(s)): %s, sense %x/%x/%x\n", what, lba, blocks,
                    l.dev->dead ? "device is gone" : "device reported an error", static_cast<unsigned>(s.key), static_cast<unsigned>(s.asc),
                    static_cast<unsigned>(s.ascq));
}

bool transfer_blocks(LunInfo& l, bool write, u64 lba, u32 blocks, void* buf) {
    if (!l.usable || l.dev->dead || blocks == 0 || blocks > MAX_SECTORS_PER_COMMAND) return false;
    if (lba >= l.blocks || blocks > l.blocks - lba) return false;
    u8 cdb[16];
    u8 length = sc::rw(cdb, write, lba, blocks);
    sc::Sense sense{SENSE_UNKNOWN, 0, 0};
    u32 moved = 0;
    bool ok = run(l, cdb, length, !write, buf, blocks * 512, &moved, &sense);
    if (ok && moved != blocks * 512) ok = false;
    if (!ok) report_error(l, write ? "write" : "read", lba, blocks, sense);
    return ok;
}

bool scan_read(void* ctx, u64 lba, void* buf512) { return transfer_blocks(*static_cast<LunInfo*>(ctx), false, lba, 1, buf512); }

struct Window : blockdev::Device {
    LunInfo* lun = nullptr;
    u64 start = 0;
    u64 sectors = 0;

    bool read_sector(u64 sector, void* buf512) override { return read_sectors(sector, 1, buf512); }
    bool write_sector(u64 sector, const void* buf512) override { return write_sectors(sector, 1, buf512); }

    bool read_sectors(u64 first, u64 count, void* buf) override {
        if (!lun || first > sectors || count > sectors - first) return false;
        u8* p = static_cast<u8*>(buf);
        while (count) {
            u32 chunk = count < MAX_SECTORS_PER_COMMAND ? static_cast<u32>(count) : MAX_SECTORS_PER_COMMAND;
            if (!transfer_blocks(*lun, false, start + first, chunk, p)) return false;
            first += chunk;
            count -= chunk;
            p += static_cast<u64>(chunk) * 512;
        }
        return true;
    }

    bool write_sectors(u64 first, u64 count, const void* buf) override {
        if (!lun || first > sectors || count > sectors - first) return false;
        u8* p = const_cast<u8*>(static_cast<const u8*>(buf));
        while (count) {
            u32 chunk = count < MAX_SECTORS_PER_COMMAND ? static_cast<u32>(count) : MAX_SECTORS_PER_COMMAND;
            if (!transfer_blocks(*lun, true, start + first, chunk, p)) return false;
            first += chunk;
            count -= chunk;
            p += static_cast<u64>(chunk) * 512;
        }
        return true;
    }

    bool flush() override {
        if (!lun || lun->dev->dead) return false;
        u8 cdb[16];
        u8 length = sc::synchronize_cache(cdb);
        sc::Sense sense{SENSE_UNKNOWN, 0, 0};
        if (run(*lun, cdb, length, false, nullptr, 0, nullptr, &sense)) return true;
        return !lun->dev->dead && sense.key == SENSE_ILLEGAL_REQUEST;
    }

    u64 capacity_sectors() override { return sectors; }
    const char* name() override { return "usb-storage"; }
};
Window g_window;

void copy_text(char* dst, const char* src, u32 cap) {
    u32 i = 0;
    for (; src[i] && i + 1 < cap; ++i) dst[i] = src[i];
    dst[i] = 0;
}

bool wait_ready(LunInfo& l) {
    for (u32 i = 0; i < READY_ATTEMPTS; ++i) {
        u8 cdb[16];
        u8 length = sc::test_unit_ready(cdb);
        sc::Sense sense{SENSE_UNKNOWN, 0, 0};
        if (run(l, cdb, length, false, nullptr, 0, nullptr, &sense)) return true;
        if (l.dev->dead) { l.note = "device stopped answering"; return false; }
        if (sense.key == SENSE_NOT_READY && sense.asc == 0x3A) { l.note = "no medium in the drive"; return false; }
        pause_ms(READY_PAUSE_MS);
    }
    l.note = "device never became ready";
    return false;
}

void read_capacity(LunInfo& l) {
    u8 cdb[16];
    u8 buf[32];
    for (u32 i = 0; i < sizeof(buf); ++i) buf[i] = 0;
    u8 length = sc::read_capacity_10(cdb);
    u32 n = 0;
    sc::Capacity cap{false, 0, 0};
    if (run(l, cdb, length, true, buf, 8, &n, nullptr) && n >= 8) cap = sc::parse_capacity_10(buf);
    if (!cap.ok) {
        for (u32 i = 0; i < sizeof(buf); ++i) buf[i] = 0;
        length = sc::read_capacity_16(cdb, 32);
        n = 0;
        if (run(l, cdb, length, true, buf, 32, &n, nullptr) && n >= 12) cap = sc::parse_capacity_16(buf);
    }
    if (!cap.ok) { l.note = "capacity could not be read"; return; }
    l.blocks = cap.blocks;
    l.block_size = cap.block_size;
    if (cap.block_size != 512) { l.note = "block size is not 512 bytes (only 512-byte media is driven)"; return; }
    if (cap.blocks < 2) { l.note = "medium is too small"; return; }
    l.usable = true;
}

void probe_lun(xhci::Device& d, u8 lun_no) {
    if (g_lun_count == MAX_LUNS) return;
    LunInfo& l = g_luns[g_lun_count++];
    l = LunInfo{};
    l.dev = &d;
    l.lun = lun_no;
    l.verdict = diskmap::Verdict::Unknown;

    u8 cdb[16];
    u8 length = sc::inquiry(cdb, 36);
    u8 buf[36];
    for (u32 i = 0; i < sizeof(buf); ++i) buf[i] = 0;
    u32 n = 0;
    if (!run(l, cdb, length, true, buf, sizeof(buf), &n, nullptr) || n < 36) { l.note = "INQUIRY failed"; return; }
    sc::Inquiry q = sc::parse_inquiry(buf);
    copy_text(l.vendor, q.vendor, sizeof(l.vendor));
    copy_text(l.product, q.product, sizeof(l.product));
    if (q.device_type != 0x00 && q.device_type != 0x0E) { l.note = "not a direct-access disk"; return; }
    if (!wait_ready(l)) return;
    l.ready = true;
    read_capacity(l);
}

void probe_device(xhci::Device& d) {
    u8 max_lun = 0;
    u8 reply = 0;
    u32 actual = 0;
    usbproto::Setup s = usbproto::setup_packet(0xA1, usbproto::REQ_BOT_GET_MAX_LUN, 0, d.storage.interface_number, 1);
    if (xhci::control(d, s, &reply, 1, &actual, CONTROL_TIMEOUT_MS) == xhci::Result::Ok && actual >= 1) max_lun = reply & 0x0F;
    if (max_lun >= MAX_LUNS) max_lun = MAX_LUNS - 1;
    if (d.dead) return;
    for (u32 lun_no = 0; lun_no <= max_lun; ++lun_no) probe_lun(d, static_cast<u8>(lun_no));
}

void identify_luns(const u8* want_guid, const char* want_volume_id) {
    for (u32 i = 0; i < g_lun_count; ++i) {
        LunInfo& l = g_luns[i];
        if (!l.usable) continue;
        diskmap::Image img;
        diskmap::scan(&scan_read, &l, l.blocks, img);
        l.scanned = true;
        l.has_image = img.used_bytes > 0;
        l.image_bytes = img.used_bytes;
        l.verdict = diskmap::identify(img, want_guid, want_volume_id);
        if (l.verdict == diskmap::Verdict::Match) {
            diskmap::Tail t = diskmap::tail(img, l.blocks);
            l.has_tail = t.ok;
            l.tail_start = t.start_sector;
            l.tail_sectors = t.sectors;
            l.note = t.ok ? "this is the stick the system booted from" : t.why;
        } else if (l.verdict == diskmap::Verdict::NotOurs) {
            l.note = "not the boot stick; left untouched";
        } else {
            l.note = "could not read the stick's first sectors";
        }
    }
}

void select_window() {
    u32 matches = 0;
    LunInfo* match = nullptr;
    for (u32 i = 0; i < g_lun_count; ++i)
        if (g_luns[i].verdict == diskmap::Verdict::Match) { ++matches; match = &g_luns[i]; }

    if (g_lun_count == 0) { g_window_status = "no USB mass-storage disk was found"; return; }
    if (matches == 0) { g_window_status = "none of the USB disks is the stick this system booted from"; return; }
    if (matches > 1) {
        for (u32 i = 0; i < g_lun_count; ++i)
            if (g_luns[i].verdict == diskmap::Verdict::Match) g_luns[i].note = "carries the same image as another stick, so the boot stick cannot be told apart";
        g_window_status = "more than one stick carries this exact image, so the booted one cannot be told apart";
        return;
    }
    if (!match->has_tail) { g_window_status = match->note; return; }
    if (blockdev::present()) { g_window_status = "another block device is already active"; return; }

    g_window.lun = match;
    g_window.start = match->tail_start;
    g_window.sectors = match->tail_sectors;
    g_window_lun = match;
    g_window_status = "ready";
    blockdev::register_device(&g_window);
}

}

void init(const u8* want_guid, const char* want_volume_id) {
    g_lun_count = 0;
    g_window_lun = nullptr;
    g_window_status = "not probed";
    g_reported_errors = 0;

    u32 devices = xhci::storage_count();
    if (devices == 0) {
        g_window_status = "no USB mass-storage device was found";
        serial::writeln("[usb-storage] no USB mass-storage device found");
        return;
    }
    for (u32 i = 0; i < devices; ++i) {
        xhci::Device* d = xhci::storage_device(i);
        if (d && !d->dead) probe_device(*d);
    }
    identify_luns(want_guid, want_volume_id);
    select_window();

    for (u32 i = 0; i < g_lun_count; ++i) {
        const LunInfo& l = g_luns[i];
        serial::printf("[usb-storage] %04x:%04x lun %u \"%s %s\" %lu sector(s): %s\n", static_cast<unsigned>(l.dev->vendor),
                        static_cast<unsigned>(l.dev->product), static_cast<unsigned>(l.lun), l.vendor, l.product, l.blocks,
                        l.note ? l.note : "ready");
    }
    if (g_window_lun) {
        serial::printf("[usb-storage] storage window: %lu sector(s) at sector %lu (%lu MiB)\n", g_window.sectors, g_window.start,
                        g_window.sectors / 2048);
    } else {
        serial::printf("[usb-storage] no storage window: %s\n", g_window_status);
    }
}

u32 lun_count() { return g_lun_count; }
const LunInfo& lun(u32 index) { return g_luns[index < MAX_LUNS ? index : 0]; }
bool window_present() { return g_window_lun != nullptr; }
const LunInfo* window_lun() { return g_window_lun; }
const char* window_status() { return g_window_status ? g_window_status : "not probed"; }

}
