#include <cosmos/xhci.hpp>
#include <cosmos/usbproto.hpp>
#include <cosmos/diskmap.hpp>
#include <stdio.h>
#include <stdlib.h>

#include "../kernel/src/drivers/blockdev.cpp"
#include "../kernel/src/drivers/usbstor.cpp"

static int g_failures = 0;

static void expect(bool cond, const char* what) {
    printf("%-84s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) ++g_failures;
}

static void put32(u8* p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put64(u8* p, u64 v) { put32(p, static_cast<u32>(v)); put32(p + 4, static_cast<u32>(v >> 32)); }

struct Fake {
    u8* data = nullptr;
    u64 sectors = 0;
    char vendor[9] = "FAKE    ";
    char product[17] = "STICK           ";
    u32 block_size = 512;
    bool write_protect = false;
    bool flush_unsupported = false;
    bool timeout_all = false;
    bool stall_next_read = false;
    bool short_write_residue = false;
    int unit_attention = 0;
    int stage = 0;
    u8 cdb[16] = {};
    u32 tag = 0;
    u32 data_len = 0;
    u8 csw_status = 0;
    u32 csw_residue = 0;
    bool stall_pending = false;
    u8* resp = nullptr;
    u32 resp_len = 0;
    u8 sense[18] = {};
    u64 pending_lba = 0;
    u32 pending_blocks = 0;
    u64 reads = 0;
    u64 write_commands = 0;
    u64 sectors_written = 0;
    u64 write_min = ~0ull;
    u64 write_max = 0;
    u64 clear_halts = 0;
    u64 resets = 0;
    u64 commands = 0;
};

static Fake g_fake[4];
static xhci::Device g_dev[4];
static u32 g_count = 0;

static const u8 GUID_A[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static const u8 GUID_B[16] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};

namespace xhci {

u32 storage_count() { return g_count; }
Device* storage_device(u32 index) { return index < g_count ? &g_dev[index] : nullptr; }

static void make_sense(Fake& f, u8 key, u8 asc, u8 ascq) {
    for (int i = 0; i < 18; ++i) f.sense[i] = 0;
    f.sense[0] = 0x70;
    f.sense[2] = key;
    f.sense[7] = 10;
    f.sense[12] = asc;
    f.sense[13] = ascq;
}

static void fail_command(Fake& f, u8 key, u8 asc, u8 ascq, bool data_in) {
    f.csw_status = 1;
    f.csw_residue = f.data_len;
    make_sense(f, key, asc, ascq);
    if (data_in && f.data_len) { f.stage = 1; f.resp_len = 0; f.stall_pending = true; }
    else f.stage = 3;
}

static void execute(Fake& f) {
    ++f.commands;
    f.csw_status = 0;
    f.csw_residue = 0;
    f.stall_pending = false;
    f.resp_len = 0;
    u8 op = f.cdb[0];
    bool in = f.data_len > 0;
    if (op == 0x00) {
        if (f.unit_attention > 0) { --f.unit_attention; fail_command(f, 6, 0x28, 0, false); return; }
        f.stage = 3;
    } else if (op == 0x03) {
        __builtin_memcpy(f.resp, f.sense, 18);
        f.resp_len = 18;
        f.csw_residue = f.data_len > 18 ? f.data_len - 18 : 0;
        f.stage = 1;
    } else if (op == 0x12) {
        for (int i = 0; i < 36; ++i) f.resp[i] = 0;
        f.resp[1] = 0x80;
        f.resp[4] = 31;
        __builtin_memcpy(f.resp + 8, f.vendor, 8);
        __builtin_memcpy(f.resp + 16, f.product, 16);
        f.resp_len = 36;
        f.stage = 1;
    } else if (op == 0x25) {
        usbproto::scsi::be32(f.resp, static_cast<u32>(f.sectors - 1));
        usbproto::scsi::be32(f.resp + 4, f.block_size);
        f.resp_len = 8;
        f.stage = 1;
    } else if (op == 0x28 || op == 0x88) {
        u64 lba;
        u32 blocks;
        if (op == 0x28) { lba = usbproto::scsi::get_be32(f.cdb + 2); blocks = (f.cdb[7] << 8) | f.cdb[8]; }
        else { lba = usbproto::scsi::get_be64(f.cdb + 2); blocks = usbproto::scsi::get_be32(f.cdb + 10); }
        ++f.reads;
        if (lba + blocks > f.sectors) { fail_command(f, 5, 0x21, 0, true); return; }
        if (f.stall_next_read) {
            f.stall_next_read = false;
            f.stall_pending = true;
            f.resp_len = 0;
            f.csw_residue = f.data_len;
            f.stage = 1;
            return;
        }
        __builtin_memcpy(f.resp, f.data + lba * 512, static_cast<u64>(blocks) * 512);
        f.resp_len = blocks * 512;
        f.stage = 1;
    } else if (op == 0x2A || op == 0x8A) {
        if (op == 0x2A) { f.pending_lba = usbproto::scsi::get_be32(f.cdb + 2); f.pending_blocks = (f.cdb[7] << 8) | f.cdb[8]; }
        else { f.pending_lba = usbproto::scsi::get_be64(f.cdb + 2); f.pending_blocks = usbproto::scsi::get_be32(f.cdb + 10); }
        ++f.write_commands;
        f.stage = 2;
    } else if (op == 0x35) {
        if (f.flush_unsupported) { fail_command(f, 5, 0x20, 0, false); return; }
        f.stage = 3;
    } else {
        fail_command(f, 5, 0x20, 0, in);
    }
    (void)in;
}

Result control(Device& d, const usbproto::Setup& s, void* data, u32 length, u32* actual, u32) {
    Fake& f = g_fake[d.slot];
    if (d.dead || f.timeout_all) { d.dead = true; return Result::Timeout; }
    if (s.request == usbproto::REQ_BOT_GET_MAX_LUN) {
        if (length < 1) return Result::Error;
        *static_cast<u8*>(data) = 0;
        if (actual) *actual = 1;
        return Result::Ok;
    }
    if (s.request == usbproto::REQ_BOT_RESET) {
        ++f.resets;
        f.stage = 0;
        return Result::Ok;
    }
    return Result::Error;
}

Result bulk_out(Device& d, const void* data, u32 length, u32) {
    Fake& f = g_fake[d.slot];
    if (d.dead || f.timeout_all) { d.dead = true; return Result::Timeout; }
    if (f.stage == 2) {
        u32 expected = f.pending_blocks * 512;
        if (length != expected) return Result::Error;
        if (f.write_protect) {
            f.csw_status = 1;
            f.csw_residue = expected;
            make_sense(f, 7, 0x27, 0);
            f.stage = 3;
            return Result::Ok;
        }
        if (f.pending_lba + f.pending_blocks > f.sectors) return Result::Error;
        __builtin_memcpy(f.data + f.pending_lba * 512, data, expected);
        f.sectors_written += f.pending_blocks;
        if (f.pending_lba < f.write_min) f.write_min = f.pending_lba;
        if (f.pending_lba + f.pending_blocks - 1 > f.write_max) f.write_max = f.pending_lba + f.pending_blocks - 1;
        f.csw_status = 0;
        f.csw_residue = f.short_write_residue ? 512 : 0;
        f.stage = 3;
        return Result::Ok;
    }
    if (f.stage != 0 || length != usbproto::CBW_BYTES) return Result::Error;
    const u8* c = static_cast<const u8*>(data);
    if (usbproto::get_le32(c) != usbproto::CBW_SIGNATURE) return Result::Error;
    f.tag = usbproto::get_le32(c + 4);
    f.data_len = usbproto::get_le32(c + 8);
    for (int i = 0; i < 16; ++i) f.cdb[i] = c[15 + i];
    execute(f);
    return Result::Ok;
}

Result bulk_in(Device& d, void* data, u32 length, u32* actual, u32) {
    Fake& f = g_fake[d.slot];
    if (d.dead || f.timeout_all) { d.dead = true; return Result::Timeout; }
    if (f.stage == 1) {
        if (f.stall_pending) {
            f.stall_pending = false;
            f.stage = 3;
            return Result::Stall;
        }
        u32 n = length < f.resp_len ? length : f.resp_len;
        __builtin_memcpy(data, f.resp, n);
        if (actual) *actual = n;
        f.stage = 3;
        return Result::Ok;
    }
    if (f.stage == 3) {
        if (length != usbproto::CSW_BYTES) return Result::Error;
        u8* c = static_cast<u8*>(data);
        put32(c, usbproto::CSW_SIGNATURE);
        put32(c + 4, f.tag);
        put32(c + 8, f.csw_residue);
        c[12] = f.csw_status;
        if (actual) *actual = usbproto::CSW_BYTES;
        f.stage = 0;
        return Result::Ok;
    }
    return Result::Error;
}

bool clear_halt(Device& d, bool) {
    Fake& f = g_fake[d.slot];
    ++f.clear_halts;
    return !d.dead;
}

}

static void add_pvd(Fake& f, const char* id, u64 blocks) {
    u8* p = f.data + 64 * 512;
    p[0] = 1;
    __builtin_memcpy(p + 1, "CD001", 5);
    p[6] = 1;
    __builtin_memset(p + 40, ' ', 32);
    __builtin_memcpy(p + 40, id, __builtin_strlen(id));
    put32(p + 80, static_cast<u32>(blocks));
    p[128] = 0x00;
    p[129] = 0x08;
}

static void add_gpt(Fake& f, const u8 guid[16], u64 alternate_lba, u32 parts, const u64 (*ranges)[2]) {
    u8* e = f.data + 2 * 512;
    for (u32 i = 0; i < parts; ++i) {
        u8* ent = e + i * 128;
        __builtin_memset(ent, 0xA5, 16);
        put64(ent + 32, ranges[i][0]);
        put64(ent + 40, ranges[i][1]);
    }
    u8* h = f.data + 512;
    __builtin_memcpy(h, "EFI PART", 8);
    put32(h + 8, 0x10000);
    put32(h + 12, 92);
    put64(h + 24, 1);
    put64(h + 32, alternate_lba);
    __builtin_memcpy(h + 56, guid, 16);
    put64(h + 72, 2);
    put32(h + 80, 128);
    put32(h + 84, 128);
    put32(h + 88, diskmap::crc32(e, 128 * 128));
    put32(h + 16, 0);
    put32(h + 16, diskmap::crc32(h, 92));
}

static void reset_world() {
    for (u32 i = 0; i < 4; ++i) {
        free(g_fake[i].data);
        free(g_fake[i].resp);
        g_fake[i] = Fake{};
        g_dev[i] = xhci::Device{};
    }
    g_count = 0;
    blockdev::g_active = nullptr;
    usbstor::g_cmd = orbital::Mutex{};
}

static Fake& add_stick(u64 mib, const u8* guid, const char* volume, bool gpt = true) {
    u32 i = g_count++;
    Fake& f = g_fake[i];
    f.sectors = mib * 2048;
    f.data = static_cast<u8*>(calloc(f.sectors, 512));
    f.resp = static_cast<u8*>(calloc(1, 70000));
    if (volume) add_pvd(f, volume, 10 * 512);
    if (gpt) {
        u64 ranges[2][2] = {{64, 20000}, {20001, 21000}};
        add_gpt(f, guid, 21500, 2, ranges);
    }
    g_dev[i].in_use = true;
    g_dev[i].slot = static_cast<u8>(i);
    g_dev[i].vendor = static_cast<u16>(0x1000 + i);
    g_dev[i].product = static_cast<u16>(0x2000 + i);
    g_dev[i].storage.found = true;
    g_dev[i].storage.interface_number = 0;
    for (u64 s = 22000; s < f.sectors; s += 7) f.data[s * 512] = 0xEE;
    return f;
}

static u32 checksum(const u8* p, u64 n) {
    u32 h = 2166136261u;
    for (u64 i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

static void selection_tests() {
    reset_world();
    Fake& boot = add_stick(64, GUID_A, "APHBOOT");
    Fake& other = add_stick(64, GUID_B, "APHOTHER");
    u32 other_before = checksum(other.data, other.sectors * 512);
    usbstor::init(GUID_A, "APHBOOT");
    expect(usbstor::lun_count() == 2, "init: both sticks are probed");
    expect(usbstor::window_present() && usbstor::window_lun() == &usbstor::lun(0), "init: the stick whose GUID matches becomes the window");
    expect(usbstor::lun(0).verdict == diskmap::Verdict::Match && usbstor::lun(1).verdict == diskmap::Verdict::NotOurs, "init: the other stick is judged not ours");
    expect(blockdev::present() && blockdev::active() == &usbstor::g_window, "init: the window is registered as the block device");
    expect(other.write_commands == 0 && other.sectors_written == 0, "init: no write command ever reaches the other stick");
    expect(boot.write_commands == 0, "init: identification and probing issue no writes to the boot stick either");
    u64 image_sectors = 21501;
    u64 expect_start = (diskmap::align_up(image_sectors * 512, diskmap::MIB) + diskmap::GUARD_BYTES) / 512;
    expect(usbstor::lun(0).tail_start == expect_start, "window: starts one guard megabyte after the image");
    expect(blockdev::capacity_sectors() == boot.sectors - expect_start, "window: runs to the end of the stick");

    u32 head_before = checksum(boot.data, expect_start * 512);
    u64 count = 300;
    u8* out = static_cast<u8*>(malloc(count * 512));
    u8* in = static_cast<u8*>(malloc(count * 512));
    for (u64 i = 0; i < count * 512; ++i) out[i] = static_cast<u8>((i * 2654435761u) >> 24);
    expect(blockdev::write_sectors(10, count, out), "window: a 300-sector write succeeds (three commands)");
    expect(blockdev::read_sectors(10, count, in) && __builtin_memcmp(in, out, count * 512) == 0, "window: the data reads back byte for byte");
    expect(boot.write_min == expect_start + 10 && boot.write_max == expect_start + 10 + count - 1, "window: writes land only inside the window, offset by its start");
    expect(boot.write_commands == 3, "window: a 300-sector write is split into 128+128+44 sector commands");
    expect(checksum(boot.data, expect_start * 512) == head_before, "window: the boot image area and guard gap are byte-for-byte unchanged");
    expect(!blockdev::write_sector(blockdev::capacity_sectors(), out), "window: writing one past the end is refused");
    expect(!blockdev::write_sectors(blockdev::capacity_sectors() - 1, 2, out), "window: a write straddling the end is refused");
    expect(!blockdev::read_sectors(blockdev::capacity_sectors(), 1, in), "window: reading past the end is refused");
    expect(blockdev::write_sectors(blockdev::capacity_sectors() - 1, 1, out), "window: the very last sector is writable");
    expect(blockdev::flush(), "window: flush succeeds");
    expect(checksum(other.data, other.sectors * 512) == other_before && other.write_commands == 0, "window: the other stick is still byte-for-byte identical");
    free(out);
    free(in);
}

static void refusal_tests() {
    static const u8 GUID_C[16] = {7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};

    reset_world();
    add_stick(64, GUID_A, "APHBOOT");
    add_stick(64, GUID_B, "APHOTHER");
    usbstor::init(GUID_B, "APHNONE");
    expect(usbstor::window_present() && usbstor::window_lun() == &usbstor::lun(1), "init: a GUID that matches the second stick selects exactly that one");
    expect(g_fake[0].write_commands == 0, "init: the first stick is untouched");

    reset_world();
    Fake& c = add_stick(64, GUID_A, "APHBOOT");
    Fake& d = add_stick(64, GUID_B, "APHOTHER");
    usbstor::init(GUID_C, "APHBOOT");
    expect(!usbstor::window_present() && !blockdev::present(), "init: when no stick matches there is no window and no block device");
    expect(c.write_commands == 0 && d.write_commands == 0, "init: nothing is written when nothing matches");

    reset_world();
    Fake& e = add_stick(64, GUID_A, "APHBOOT");
    Fake& g = add_stick(64, GUID_A, "APHBOOT");
    usbstor::init(GUID_A, "APHBOOT");
    expect(!usbstor::window_present() && !blockdev::present(), "init: two sticks carrying the same image give no window (cannot tell which booted)");
    expect(e.write_commands == 0 && g.write_commands == 0, "init: and neither is written");

    reset_world();
    Fake& h = add_stick(64, nullptr, "APHBOOT", false);
    h.data[510] = 0x55;
    h.data[511] = 0xAA;
    u8* ent = h.data + 446;
    ent[4] = 0x0C;
    put32(ent + 8, 2048);
    put32(ent + 12, static_cast<u32>(h.sectors - 2048));
    usbstor::init(nullptr, "APHBOOT");
    expect(usbstor::lun(0).verdict == diskmap::Verdict::Match && !usbstor::window_present(), "init: a Rufus ISO-mode stick matches by volume id but gets no window");
    expect(!blockdev::present() && h.write_commands == 0, "init: and nothing is registered or written");

    reset_world();
    Fake& i = add_stick(64, nullptr, "APHBOOT", false);
    usbstor::init(nullptr, "APHBOOT");
    expect(usbstor::window_present(), "init: the volume id alone identifies a stick without a GPT when Limine gave no GUID");
    expect(i.write_commands == 0, "init: still no writes");

    reset_world();
    Fake& j = add_stick(64, GUID_A, "APHBOOT");
    j.block_size = 4096;
    usbstor::init(GUID_A, "APHBOOT");
    expect(!usbstor::lun(0).usable && !usbstor::window_present(), "init: a 4096-byte-sector stick is reported unusable and gets no window");
    expect(j.reads == 0 && j.write_commands == 0, "init: no data sector of an unsupported stick is read or written");

    reset_world();
    Fake& k = add_stick(64, GUID_A, "APHBOOT");
    k.timeout_all = true;
    usbstor::init(GUID_A, "APHBOOT");
    expect(!usbstor::window_present() && !blockdev::present(), "init: a stick that stops answering ends probing cleanly with no window");

    reset_world();
    usbstor::init(GUID_A, "APHBOOT");
    expect(!usbstor::window_present() && usbstor::lun_count() == 0, "init: with no USB storage at all there is simply no window");
}

static void error_path_tests() {
    reset_world();
    Fake& f = add_stick(64, GUID_A, "APHBOOT");
    f.unit_attention = 2;
    usbstor::init(GUID_A, "APHBOOT");
    expect(usbstor::window_present(), "errors: unit attention conditions are retried and the stick still comes up");

    f.stall_next_read = true;
    u8 buf[512];
    u64 clears_before = f.clear_halts;
    bool first = blockdev::read_sector(5, buf);
    expect(!first && f.clear_halts > clears_before, "errors: a stalled read that moved no data fails and clears the halt");
    expect(blockdev::read_sector(5, buf), "errors: the next read works, so the protocol stayed in sync after the stall");

    f.short_write_residue = true;
    expect(!blockdev::write_sector(5, buf), "errors: a write the device only partly accepted is reported as failed");
    f.short_write_residue = false;

    f.write_protect = true;
    expect(!blockdev::write_sector(5, buf), "errors: a write-protected stick fails writes");
    expect(blockdev::read_sector(5, buf) && !g_dev[0].dead, "errors: and reads keep working afterwards");
    f.write_protect = false;

    f.flush_unsupported = true;
    expect(blockdev::flush(), "errors: a stick that does not implement SYNCHRONIZE CACHE still flushes cleanly");

    g_dev[0].dead = true;
    expect(!blockdev::read_sector(5, buf) && !blockdev::write_sector(5, buf) && !blockdev::flush(), "errors: once the device is gone every operation fails fast");
}

int main() {
    selection_tests();
    refusal_tests();
    error_path_tests();
    printf("\n%s\n", g_failures ? "FAILURES" : "all usbstor host tests passed");
    return g_failures ? 1 : 0;
}
