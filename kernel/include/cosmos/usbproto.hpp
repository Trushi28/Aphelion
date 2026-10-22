#pragma once
#include <cosmos/types.hpp>

namespace usbproto {

constexpr u8 REQ_GET_STATUS = 0;
constexpr u8 REQ_CLEAR_FEATURE = 1;
constexpr u8 REQ_GET_DESCRIPTOR = 6;
constexpr u8 REQ_SET_CONFIGURATION = 9;
constexpr u8 REQ_BOT_GET_MAX_LUN = 0xFE;
constexpr u8 REQ_BOT_RESET = 0xFF;

constexpr u8 DESC_DEVICE = 1;
constexpr u8 DESC_CONFIG = 2;
constexpr u8 DESC_INTERFACE = 4;
constexpr u8 DESC_ENDPOINT = 5;
constexpr u8 DESC_SS_COMPANION = 0x30;

constexpr u8 CLASS_MASS_STORAGE = 0x08;
constexpr u8 SUBCLASS_SCSI = 0x06;
constexpr u8 SUBCLASS_RBC = 0x01;
constexpr u8 PROTOCOL_BOT = 0x50;

struct PACKED Setup {
    u8 request_type;
    u8 request;
    u16 value;
    u16 index;
    u16 length;
};

constexpr Setup setup_packet(u8 type, u8 request, u16 value, u16 index, u16 length) { return Setup{type, request, value, index, length}; }

struct MassStorage {
    bool found;
    u8 configuration;
    u8 interface_number;
    u8 bulk_in;
    u8 bulk_out;
    u16 in_max_packet;
    u16 out_max_packet;
    u8 in_burst;
    u8 out_burst;
};

inline MassStorage find_mass_storage(const u8* cfg, u32 length) {
    MassStorage r{};
    if (length < 9 || cfg[1] != DESC_CONFIG) return r;
    u32 total = static_cast<u32>(cfg[2] | (cfg[3] << 8));
    if (total > length) total = length;
    u8 configuration = cfg[5];

    bool in_candidate = false;
    u8 iface = 0;
    int pending = -1;
    MassStorage cur{};
    u32 off = cfg[0];
    while (off + 2 <= total) {
        u8 len = cfg[off], type = cfg[off + 1];
        if (len < 2 || off + len > total) break;
        if (type == DESC_INTERFACE && len >= 9) {
            in_candidate = cfg[off + 5] == CLASS_MASS_STORAGE && (cfg[off + 6] == SUBCLASS_SCSI || cfg[off + 6] == SUBCLASS_RBC) && cfg[off + 7] == PROTOCOL_BOT && cfg[off + 3] == 0;
            iface = cfg[off + 2];
            cur = MassStorage{};
            cur.configuration = configuration;
            cur.interface_number = iface;
            pending = -1;
        } else if (type == DESC_ENDPOINT && len >= 7 && in_candidate) {
            u8 addr = cfg[off + 2];
            u8 attrs = cfg[off + 3];
            u16 mps = static_cast<u16>(cfg[off + 4] | (cfg[off + 5] << 8));
            if ((attrs & 3) == 2) {
                if (addr & 0x80) { if (!cur.bulk_in) { cur.bulk_in = addr & 0x0F; cur.in_max_packet = mps & 0x7FF; pending = 1; } else pending = -1; }
                else { if (!cur.bulk_out) { cur.bulk_out = addr & 0x0F; cur.out_max_packet = mps & 0x7FF; pending = 0; } else pending = -1; }
            } else {
                pending = -1;
            }
        } else if (type == DESC_SS_COMPANION && len >= 6 && in_candidate && pending >= 0) {
            if (pending == 1) cur.in_burst = cfg[off + 2];
            else cur.out_burst = cfg[off + 2];
            pending = -1;
        }
        if (in_candidate && cur.bulk_in && cur.bulk_out) {
            u32 next = off + len;
            bool companion_follows = next + 2 <= total && cfg[next + 1] == DESC_SS_COMPANION && pending >= 0;
            if (!companion_follows) {
                cur.found = true;
                return cur;
            }
        }
        off += len;
    }
    return r;
}

constexpr u32 CBW_SIGNATURE = 0x43425355;
constexpr u32 CSW_SIGNATURE = 0x53425355;
constexpr u32 CBW_BYTES = 31;
constexpr u32 CSW_BYTES = 13;

inline void put_le32(u8* p, u32 v) { p[0] = static_cast<u8>(v); p[1] = static_cast<u8>(v >> 8); p[2] = static_cast<u8>(v >> 16); p[3] = static_cast<u8>(v >> 24); }
inline u32 get_le32(const u8* p) { return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24); }

inline void build_cbw(u8* out, u32 tag, u32 data_length, bool device_to_host, u8 lun, const u8* cdb, u8 cdb_length) {
    for (u32 i = 0; i < CBW_BYTES; ++i) out[i] = 0;
    put_le32(out, CBW_SIGNATURE);
    put_le32(out + 4, tag);
    put_le32(out + 8, data_length);
    out[12] = device_to_host ? 0x80 : 0x00;
    out[13] = lun & 0x0F;
    out[14] = cdb_length;
    for (u8 i = 0; i < cdb_length && i < 16; ++i) out[15 + i] = cdb[i];
}

enum class CswResult : u8 { Passed, Failed, PhaseError, Invalid };

struct Csw {
    CswResult result;
    u32 residue;
};

inline Csw parse_csw(const u8* in, u32 length, u32 expected_tag) {
    Csw c{CswResult::Invalid, 0};
    if (length != CSW_BYTES || get_le32(in) != CSW_SIGNATURE || get_le32(in + 4) != expected_tag) return c;
    c.residue = get_le32(in + 8);
    switch (in[12]) {
        case 0: c.result = CswResult::Passed; break;
        case 1: c.result = CswResult::Failed; break;
        case 2: c.result = CswResult::PhaseError; break;
        default: c.result = CswResult::Invalid; break;
    }
    return c;
}

namespace scsi {

constexpr u8 TEST_UNIT_READY = 0x00;
constexpr u8 REQUEST_SENSE = 0x03;
constexpr u8 INQUIRY = 0x12;
constexpr u8 READ_CAPACITY_10 = 0x25;
constexpr u8 READ_10 = 0x28;
constexpr u8 WRITE_10 = 0x2A;
constexpr u8 SYNCHRONIZE_CACHE_10 = 0x35;
constexpr u8 READ_16 = 0x88;
constexpr u8 WRITE_16 = 0x8A;
constexpr u8 SERVICE_ACTION_IN_16 = 0x9E;
constexpr u8 READ_CAPACITY_16_ACTION = 0x10;

inline void be32(u8* p, u32 v) { p[0] = static_cast<u8>(v >> 24); p[1] = static_cast<u8>(v >> 16); p[2] = static_cast<u8>(v >> 8); p[3] = static_cast<u8>(v); }
inline void be64(u8* p, u64 v) { be32(p, static_cast<u32>(v >> 32)); be32(p + 4, static_cast<u32>(v)); }
inline u32 get_be32(const u8* p) { return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) | (static_cast<u32>(p[2]) << 8) | p[3]; }
inline u64 get_be64(const u8* p) { return (static_cast<u64>(get_be32(p)) << 32) | get_be32(p + 4); }

inline u8 test_unit_ready(u8* cdb) { for (int i = 0; i < 6; ++i) cdb[i] = 0; return 6; }
inline u8 request_sense(u8* cdb, u8 length) { for (int i = 0; i < 6; ++i) cdb[i] = 0; cdb[0] = REQUEST_SENSE; cdb[4] = length; return 6; }
inline u8 inquiry(u8* cdb, u8 length) { for (int i = 0; i < 6; ++i) cdb[i] = 0; cdb[0] = INQUIRY; cdb[4] = length; return 6; }
inline u8 read_capacity_10(u8* cdb) { for (int i = 0; i < 10; ++i) cdb[i] = 0; cdb[0] = READ_CAPACITY_10; return 10; }
inline u8 read_capacity_16(u8* cdb, u32 allocation) {
    for (int i = 0; i < 16; ++i) cdb[i] = 0;
    cdb[0] = SERVICE_ACTION_IN_16;
    cdb[1] = READ_CAPACITY_16_ACTION;
    be32(cdb + 10, allocation);
    return 16;
}
inline u8 synchronize_cache(u8* cdb) { for (int i = 0; i < 10; ++i) cdb[i] = 0; cdb[0] = SYNCHRONIZE_CACHE_10; return 10; }

inline u8 rw(u8* cdb, bool write, u64 lba, u32 blocks) {
    if (lba + blocks <= 0x100000000ull && blocks <= 0xFFFF) {
        for (int i = 0; i < 10; ++i) cdb[i] = 0;
        cdb[0] = write ? WRITE_10 : READ_10;
        be32(cdb + 2, static_cast<u32>(lba));
        cdb[7] = static_cast<u8>(blocks >> 8);
        cdb[8] = static_cast<u8>(blocks);
        return 10;
    }
    for (int i = 0; i < 16; ++i) cdb[i] = 0;
    cdb[0] = write ? WRITE_16 : READ_16;
    be64(cdb + 2, lba);
    be32(cdb + 10, blocks);
    return 16;
}

struct Capacity {
    bool ok;
    u64 blocks;
    u32 block_size;
};

inline Capacity parse_capacity_10(const u8* d) {
    u32 last = get_be32(d);
    return Capacity{last != 0xFFFFFFFFu, static_cast<u64>(last) + 1, get_be32(d + 4)};
}

inline Capacity parse_capacity_16(const u8* d) {
    u64 last = get_be64(d);
    return Capacity{last != ~0ull, last + 1, get_be32(d + 8)};
}

struct Inquiry {
    u8 device_type;
    bool removable;
    char vendor[9];
    char product[17];
};

inline void trim(char* s, int n) {
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == 0)) --n;
    s[n] = 0;
}

inline Inquiry parse_inquiry(const u8* d) {
    Inquiry q{};
    q.device_type = d[0] & 0x1F;
    q.removable = (d[1] & 0x80) != 0;
    for (int i = 0; i < 8; ++i) q.vendor[i] = d[8 + i] >= 0x20 && d[8 + i] < 0x7F ? static_cast<char>(d[8 + i]) : ' ';
    for (int i = 0; i < 16; ++i) q.product[i] = d[16 + i] >= 0x20 && d[16 + i] < 0x7F ? static_cast<char>(d[16 + i]) : ' ';
    trim(q.vendor, 8);
    trim(q.product, 16);
    return q;
}

struct Sense {
    u8 key;
    u8 asc;
    u8 ascq;
};

inline Sense parse_sense(const u8* d, u32 length) {
    Sense s{0, 0, 0};
    if (length < 14) return s;
    u8 response = d[0] & 0x7F;
    if (response == 0x70 || response == 0x71) { s.key = d[2] & 0x0F; s.asc = d[12]; s.ascq = d[13]; }
    else if ((response == 0x72 || response == 0x73) && length >= 4) { s.key = d[1] & 0x0F; s.asc = d[2]; s.ascq = d[3]; }
    return s;
}

}

}
