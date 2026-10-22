#include <cosmos/diskmap.hpp>
#include <cosmos/usbproto.hpp>
#include <stdio.h>
#include <stdlib.h>


static int g_failures = 0;

static void expect(bool cond, const char* what) {
    printf("%-76s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) ++g_failures;
}

struct Disk {
    u8* data;
    u64 sectors;
    u64 reads = 0;
};

static bool read_cb(void* ctx, u64 lba, void* buf) {
    Disk* d = static_cast<Disk*>(ctx);
    ++d->reads;
    if (lba >= d->sectors) return false;
    __builtin_memcpy(buf, d->data + lba * 512, 512);
    return true;
}

static void put32(u8* p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put64(u8* p, u64 v) { put32(p, static_cast<u32>(v)); put32(p + 4, static_cast<u32>(v >> 32)); }

static Disk make_disk(u64 mib) {
    Disk d;
    d.sectors = mib * 2048;
    d.data = static_cast<u8*>(calloc(d.sectors, 512));
    return d;
}

static void add_pvd(Disk& d, const char* id, u64 blocks) {
    u8* p = d.data + 64 * 512;
    p[0] = 1;
    __builtin_memcpy(p + 1, "CD001", 5);
    p[6] = 1;
    __builtin_memset(p + 40, ' ', 32);
    __builtin_memcpy(p + 40, id, __builtin_strlen(id));
    put32(p + 80, static_cast<u32>(blocks));
    p[128] = 0x00;
    p[129] = 0x08;
}

static void add_gpt(Disk& d, const u8 guid[16], u64 alternate_lba, u32 parts, const u64 (*ranges)[2], bool fix_crc = true) {
    u8* e = d.data + 2 * 512;
    for (u32 i = 0; i < parts; ++i) {
        u8* ent = e + i * 128;
        __builtin_memset(ent, 0xA5, 16);
        put64(ent + 32, ranges[i][0]);
        put64(ent + 40, ranges[i][1]);
    }
    u8* h = d.data + 512;
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
    if (!fix_crc) h[20] ^= 1;
}

static const u8 GUID_A[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static const u8 GUID_B[16] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};

static void crc_tests() {
    const u8 s[] = "123456789";
    expect(diskmap::crc32(s, 9) == 0xCBF43926u, "crc32: the standard check value");
    expect(diskmap::crc32(s, 4, 0) != diskmap::crc32(s, 5, 0), "crc32: different input, different value");
}

static void iso_hybrid_tests() {
    Disk d = make_disk(256);
    add_pvd(d, "APH0123456789AB", 10 * 512);
    u64 ranges[2][2] = {{64, 40000}, {70000, 71000}};
    add_gpt(d, GUID_A, 71500, 2, ranges);
    diskmap::Image img;
    diskmap::scan(read_cb, &d, d.sectors, img);
    expect(img.readable && img.gpt && !img.gpt_damaged && img.pvd && img.partitions == 2, "scan: a hybrid ISO shows a GPT, two partitions and a volume descriptor");
    expect(__builtin_strcmp(img.volume_id, "APH0123456789AB") == 0, "scan: the volume id is read and trimmed");
    expect(img.used_bytes == 71501ull * 512, "scan: the image ends at the backup GPT, past the last partition");
    diskmap::Tail t = diskmap::tail(img, d.sectors);
    u64 expect_start = (diskmap::align_up(71501ull * 512, diskmap::MIB) + diskmap::GUARD_BYTES) / 512;
    expect(t.ok && t.start_sector == expect_start && t.start_sector * 512 % diskmap::MIB == 0, "tail: starts a megabyte-aligned guard gap after the image");
    expect(t.start_sector + t.sectors == d.sectors, "tail: runs to the end of the stick");
    expect(diskmap::identify(img, GUID_A, "APH0123456789AB") == diskmap::Verdict::Match, "identify: the Limine disk GUID matches");
    expect(diskmap::identify(img, GUID_B, "APH0123456789AB") == diskmap::Verdict::NotOurs, "identify: a different GUID is rejected even when the volume id matches");
    expect(diskmap::identify(img, nullptr, "APH0123456789AB") == diskmap::Verdict::Match, "identify: without a GUID the build's volume id decides");
    expect(diskmap::identify(img, nullptr, "APHFFFFFFFFFFFF") == diskmap::Verdict::NotOurs, "identify: another build's volume id is rejected");
    expect(diskmap::identify(img, nullptr, nullptr) == diskmap::Verdict::NotOurs, "identify: nothing to compare against never matches");
    free(d.data);
}

static void pvd_only_tests() {
    Disk d = make_disk(128);
    add_pvd(d, "APH0123456789AB", 30 * 512);
    diskmap::Image img;
    diskmap::scan(read_cb, &d, d.sectors, img);
    expect(!img.gpt && img.pvd && img.used_bytes == 30ull * 512 * 2048, "scan: an ISO without partition tables ends at its volume size");
    expect(diskmap::identify(img, GUID_A, "APH0123456789AB") == diskmap::Verdict::Match, "identify: a stick with no GPT falls back to the volume id even if Limine reported a GUID");
    diskmap::Tail t = diskmap::tail(img, d.sectors);
    expect(t.ok && t.start_sector * 512 >= 30ull * 512 * 2048 + diskmap::GUARD_BYTES, "tail: leaves the guard gap after the volume");
    free(d.data);
}

static void protective_tests() {
    Disk d = make_disk(64);
    diskmap::Image img;
    diskmap::scan(read_cb, &d, d.sectors, img);
    expect(img.readable && img.used_bytes == 0 && !diskmap::tail(img, d.sectors).ok, "tail: a blank stick has no recognisable image and gets no window");
    expect(diskmap::identify(img, GUID_A, "x") == diskmap::Verdict::NotOurs, "identify: a blank stick is not ours");
    free(d.data);
}

static void refusal_tests() {
    {
        Disk d = make_disk(64);
        d.data[510] = 0x55; d.data[511] = 0xAA;
        u8* e = d.data + 446;
        e[4] = 0x0C;
        put32(e + 8, 2048);
        put32(e + 12, d.sectors - 2048);
        diskmap::Image img;
        diskmap::scan(read_cb, &d, d.sectors, img);
        expect(img.mbr_signature && img.partitions == 1 && img.used_bytes == d.sectors * 512, "scan: an MBR partition covering the stick uses all of it");
        expect(!diskmap::tail(img, d.sectors).ok, "tail: a Rufus ISO-mode stick (one big FAT partition) gets no window");
        free(d.data);
    }
    {
        Disk d = make_disk(128);
        u64 ranges[1][2] = {{2048, 20000}};
        add_gpt(d, GUID_A, d.sectors - 1, 1, ranges);
        diskmap::Image img;
        diskmap::scan(read_cb, &d, d.sectors, img);
        expect(img.gpt && img.used_bytes == d.sectors * 512, "scan: a GPT whose backup header is at the end of the stick claims the whole stick");
        diskmap::Tail t = diskmap::tail(img, d.sectors);
        expect(!t.ok && t.why, "tail: and so leaves no window, with a reason");
        free(d.data);
    }
    {
        Disk d = make_disk(128);
        u64 ranges[1][2] = {{2048, 20000}};
        add_gpt(d, GUID_A, 25000, 1, ranges, false);
        diskmap::Image img;
        diskmap::scan(read_cb, &d, d.sectors, img);
        expect(img.gpt_damaged && !img.gpt && !diskmap::tail(img, d.sectors).ok, "scan: a GPT header with a bad checksum is treated as damaged and gets no window");
        free(d.data);
    }
    {
        Disk d = make_disk(128);
        u64 ranges[1][2] = {{2048, 20000}};
        add_gpt(d, GUID_A, 25000, 1, ranges);
        d.data[2 * 512 + 3] ^= 0xFF;
        diskmap::Image img;
        diskmap::scan(read_cb, &d, d.sectors, img);
        expect(img.gpt_damaged && !diskmap::tail(img, d.sectors).ok, "scan: a bad partition-array checksum is treated as damaged");
        free(d.data);
    }
    {
        Disk d = make_disk(20);
        add_pvd(d, "X", 4 * 512);
        diskmap::Image img;
        diskmap::scan(read_cb, &d, d.sectors, img);
        expect(!diskmap::tail(img, d.sectors).ok, "tail: less than 16 MiB of free space is not worth a window");
        free(d.data);
    }
    {
        Disk d = make_disk(1);
        diskmap::Image img;
        diskmap::scan(read_cb, &d, 1, img);
        expect(!img.readable, "scan: a one-sector stick is unreadable, not a crash");
        free(d.data);
    }
}

static void config_tests() {
    const u8 cfg[] = {
        9, 2, 44, 0, 1, 1, 0, 0x80, 50,
        9, 4, 0, 0, 2, 8, 6, 0x50, 0,
        7, 5, 0x81, 2, 0x00, 0x02, 0,
        6, 0x30, 4, 0, 0, 0,
        7, 5, 0x02, 2, 0x00, 0x02, 0,
        6, 0x30, 15, 0, 0, 0,
    };
    usbproto::MassStorage m = usbproto::find_mass_storage(cfg, sizeof(cfg));
    expect(m.found && m.bulk_in == 1 && m.bulk_out == 2 && m.interface_number == 0 && m.configuration == 1, "usb: a SuperSpeed mass-storage configuration yields both bulk endpoints");
    expect(m.in_max_packet == 512 && m.out_max_packet == 512 && m.in_burst == 4 && m.out_burst == 15, "usb: packet sizes and burst values come from the endpoint companions");

    const u8 hs[] = {
        9, 2, 32, 0, 1, 1, 0, 0x80, 50,
        9, 4, 0, 0, 2, 8, 6, 0x50, 0,
        7, 5, 0x02, 2, 0x00, 0x02, 0,
        7, 5, 0x81, 2, 0x00, 0x02, 0,
    };
    m = usbproto::find_mass_storage(hs, sizeof(hs));
    expect(m.found && m.bulk_in == 1 && m.bulk_out == 2 && m.in_burst == 0, "usb: a high-speed configuration with endpoints in the other order");

    const u8 uas[] = {
        9, 2, 55, 0, 1, 1, 0, 0x80, 50,
        9, 4, 0, 0, 2, 8, 6, 0x50, 0,
        7, 5, 0x81, 2, 0x00, 0x02, 0,
        7, 5, 0x02, 2, 0x00, 0x02, 0,
        9, 4, 0, 1, 2, 8, 6, 0x62, 0,
        7, 5, 0x83, 2, 0x00, 0x02, 0,
        7, 5, 0x04, 2, 0x00, 0x02, 0,
    };
    m = usbproto::find_mass_storage(uas, sizeof(uas));
    expect(m.found && m.bulk_in == 1 && m.bulk_out == 2 && m.interface_number == 0, "usb: a bulk-only first setting is used and the UAS alternate is never selected");

    const u8 uas_only[] = {
        9, 2, 41, 0, 1, 1, 0, 0x80, 50,
        9, 4, 0, 0, 2, 8, 6, 0x62, 0,
        7, 5, 0x81, 2, 0x00, 0x02, 0,
        7, 5, 0x02, 2, 0x00, 0x02, 0,
    };
    expect(!usbproto::find_mass_storage(uas_only, sizeof(uas_only)).found, "usb: a UAS-only device is not claimed");

    const u8 hid[] = {
        9, 2, 25, 0, 1, 1, 0, 0x80, 50,
        9, 4, 0, 0, 1, 3, 1, 1, 0,
        7, 5, 0x81, 3, 8, 0, 10,
    };
    expect(!usbproto::find_mass_storage(hid, sizeof(hid)).found, "usb: a keyboard is not a mass-storage device");
    expect(!usbproto::find_mass_storage(cfg, 5).found, "usb: a truncated descriptor is not a crash");
    u8 broken[sizeof(cfg)];
    __builtin_memcpy(broken, cfg, sizeof(cfg));
    broken[9] = 0;
    expect(!usbproto::find_mass_storage(broken, sizeof(broken)).found, "usb: a zero-length descriptor stops the walk");
}

static void bot_tests() {
    u8 cdb[16];
    u8 n = usbproto::scsi::rw(cdb, false, 0x12345678, 8);
    expect(n == 10 && cdb[0] == 0x28 && cdb[2] == 0x12 && cdb[5] == 0x78 && cdb[8] == 8, "scsi: READ(10) encodes the block address and count big-endian");
    n = usbproto::scsi::rw(cdb, true, 0x1'0000'0000ull, 8);
    expect(n == 16 && cdb[0] == 0x8A && cdb[5] == 1 && cdb[9] == 0 && cdb[13] == 8, "scsi: addresses past 2 TiB switch to WRITE(16)");
    n = usbproto::scsi::rw(cdb, false, 0xFFFFFFF0ull, 0x20);
    expect(n == 16 && cdb[0] == 0x88, "scsi: a transfer that crosses the 32-bit boundary uses READ(16)");
    n = usbproto::scsi::rw(cdb, true, 5, 128);
    expect(n == 10 && cdb[0] == 0x2A && cdb[7] == 0 && cdb[8] == 128, "scsi: WRITE(10) of 128 blocks");
    n = usbproto::scsi::read_capacity_16(cdb, 32);
    expect(n == 16 && cdb[0] == 0x9E && cdb[1] == 0x10 && cdb[13] == 32, "scsi: READ CAPACITY(16)");

    u8 cbw[31];
    usbproto::build_cbw(cbw, 0xAABBCCDD, 65536, true, 2, cdb, 10);
    expect(cbw[0] == 'U' && cbw[1] == 'S' && cbw[2] == 'B' && cbw[3] == 'C' && cbw[4] == 0xDD && cbw[7] == 0xAA, "bot: the CBW starts with USBC and the tag");
    expect(cbw[8] == 0 && cbw[9] == 0 && cbw[10] == 1 && cbw[12] == 0x80 && cbw[13] == 2 && cbw[14] == 10, "bot: length, direction, LUN and CDB length");

    u8 csw[13] = {'U', 'S', 'B', 'S', 0x01, 0, 0, 0, 0, 2, 0, 0, 0};
    usbproto::Csw c = usbproto::parse_csw(csw, 13, 1);
    expect(c.result == usbproto::CswResult::Passed && c.residue == 0x200, "bot: a good CSW carries its residue");
    csw[12] = 1;
    expect(usbproto::parse_csw(csw, 13, 1).result == usbproto::CswResult::Failed, "bot: status 1 is a failed command");
    csw[12] = 2;
    expect(usbproto::parse_csw(csw, 13, 1).result == usbproto::CswResult::PhaseError, "bot: status 2 is a phase error");
    csw[12] = 0;
    expect(usbproto::parse_csw(csw, 13, 2).result == usbproto::CswResult::Invalid, "bot: a wrong tag is invalid");
    expect(usbproto::parse_csw(csw, 12, 1).result == usbproto::CswResult::Invalid, "bot: a short CSW is invalid");
    csw[0] = 'X';
    expect(usbproto::parse_csw(csw, 13, 1).result == usbproto::CswResult::Invalid, "bot: a wrong signature is invalid");

    u8 cap10[8] = {0x00, 0x1D, 0x1A, 0xFF, 0, 0, 2, 0};
    usbproto::scsi::Capacity cap = usbproto::scsi::parse_capacity_10(cap10);
    expect(cap.ok && cap.blocks == 0x1D1B00ull && cap.block_size == 512, "scsi: capacity from READ CAPACITY(10)");
    u8 big[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 2, 0};
    expect(!usbproto::scsi::parse_capacity_10(big).ok, "scsi: a saturated READ CAPACITY(10) asks for the 16-byte form");
    u8 cap16[32] = {0, 0, 0, 1, 0, 0, 0, 0xFF, 0, 0, 0x10, 0};
    cap = usbproto::scsi::parse_capacity_16(cap16);
    expect(cap.ok && cap.blocks == 0x1'0000'0100ull && cap.block_size == 4096, "scsi: capacity from READ CAPACITY(16)");

    u8 inq[36] = {0};
    inq[1] = 0x80;
    __builtin_memcpy(inq + 8, "SanDisk ", 8);
    __builtin_memcpy(inq + 16, "Ultra Fit       ", 16);
    usbproto::scsi::Inquiry q = usbproto::scsi::parse_inquiry(inq);
    expect(q.device_type == 0 && q.removable && __builtin_strcmp(q.vendor, "SanDisk") == 0 && __builtin_strcmp(q.product, "Ultra Fit") == 0, "scsi: INQUIRY vendor and product are trimmed");
    u8 sense[18] = {0x70, 0, 0x06, 0, 0, 0, 0, 10, 0, 0, 0, 0, 0x28, 0x00};
    usbproto::scsi::Sense s = usbproto::scsi::parse_sense(sense, 18);
    expect(s.key == 6 && s.asc == 0x28 && s.ascq == 0, "scsi: fixed-format sense data");
    expect(usbproto::scsi::parse_sense(sense, 8).key == 0, "scsi: truncated sense data reads as no sense");

    usbproto::Setup su = usbproto::setup_packet(0x80, 6, 0x0100, 0, 18);
    expect(sizeof(su) == 8 && reinterpret_cast<u8*>(&su)[0] == 0x80 && reinterpret_cast<u8*>(&su)[3] == 0x01 && reinterpret_cast<u8*>(&su)[6] == 18, "usb: the setup packet is eight packed bytes");
}

int main() {
    crc_tests();
    iso_hybrid_tests();
    pvd_only_tests();
    protective_tests();
    refusal_tests();
    config_tests();
    bot_tests();
    return g_failures ? 1 : 0;
}
