#pragma once
#include <cosmos/types.hpp>

namespace diskmap {

constexpr u64 SECTOR = 512;
constexpr u64 MIB = 1ull << 20;
constexpr u64 GUARD_BYTES = MIB;
constexpr u64 MIN_TAIL_BYTES = 16 * MIB;
constexpr u32 MAX_GPT_ENTRIES = 128;
constexpr u64 PVD_LBA = 64;

using ReadFn = bool (*)(void* ctx, u64 lba, void* buf512);

struct Image {
    bool readable;
    bool mbr_signature;
    bool gpt;
    bool gpt_damaged;
    u8 disk_guid[16];
    bool pvd;
    char volume_id[33];
    u32 partitions;
    u64 used_bytes;
};

struct Tail {
    bool ok;
    u64 start_sector;
    u64 sectors;
    const char* why;
};

inline u32 crc32(const u8* p, u64 n, u32 crc = 0) {
    crc = ~crc;
    for (u64 i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

inline u16 le16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
inline u32 le32(const u8* p) { return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24); }
inline u64 le64(const u8* p) { return le32(p) | (static_cast<u64>(le32(p + 4)) << 32); }

inline u64 align_up(u64 v, u64 a) { return (v + a - 1) / a * a; }

inline void raise(Image& img, u64 bytes) { if (bytes > img.used_bytes) img.used_bytes = bytes; }

inline void scan_gpt(ReadFn read, void* ctx, u64 capacity_sectors, Image& img, const u8* lba1) {
    static const u8 SIG[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
    for (int i = 0; i < 8; ++i) if (lba1[i] != SIG[i]) return;

    u64 capacity = capacity_sectors * SECTOR;
    u32 header_size = le32(lba1 + 12);
    u32 header_crc = le32(lba1 + 16);
    u8 copy[SECTOR];
    for (u64 i = 0; i < SECTOR; ++i) copy[i] = lba1[i];
    if (header_size < 92 || header_size > SECTOR) { img.gpt_damaged = true; raise(img, capacity); return; }
    copy[16] = copy[17] = copy[18] = copy[19] = 0;
    if (crc32(copy, header_size) != header_crc || le64(lba1 + 24) != 1) { img.gpt_damaged = true; raise(img, capacity); return; }

    u64 alternate = le64(lba1 + 32);
    u64 entries_lba = le64(lba1 + 72);
    u32 count = le32(lba1 + 80);
    u32 entry_size = le32(lba1 + 84);
    u32 entries_crc = le32(lba1 + 88);
    if (alternate >= capacity_sectors) raise(img, capacity);
    else raise(img, (alternate + 1) * SECTOR);

    if (count == 0 || count > MAX_GPT_ENTRIES || entry_size != 128 || entries_lba < 2 || entries_lba + (static_cast<u64>(count) * entry_size + SECTOR - 1) / SECTOR > capacity_sectors) {
        img.gpt_damaged = true;
        raise(img, capacity);
        return;
    }

    u8 table[MAX_GPT_ENTRIES * 128];
    u64 bytes = static_cast<u64>(count) * entry_size;
    u64 sectors = (bytes + SECTOR - 1) / SECTOR;
    u8 sector[SECTOR];
    for (u64 s = 0; s < sectors; ++s) {
        if (!read(ctx, entries_lba + s, sector)) { img.gpt_damaged = true; raise(img, capacity); return; }
        for (u64 i = 0; i < SECTOR && s * SECTOR + i < sizeof(table); ++i) table[s * SECTOR + i] = sector[i];
    }
    if (crc32(table, bytes) != entries_crc) { img.gpt_damaged = true; raise(img, capacity); return; }

    img.gpt = true;
    for (int i = 0; i < 16; ++i) img.disk_guid[i] = lba1[56 + i];
    for (u32 e = 0; e < count; ++e) {
        const u8* ent = table + static_cast<u64>(e) * entry_size;
        bool used = false;
        for (int i = 0; i < 16; ++i) if (ent[i]) used = true;
        if (!used) continue;
        u64 first = le64(ent + 32), last = le64(ent + 40);
        if (last < first || last >= capacity_sectors) { raise(img, capacity); continue; }
        ++img.partitions;
        raise(img, (last + 1) * SECTOR);
    }
}

inline void scan_pvd(ReadFn read, void* ctx, u64 capacity_sectors, Image& img) {
    if (capacity_sectors < PVD_LBA + 4) return;
    u8 block[2048];
    for (u64 s = 0; s < 4; ++s)
        if (!read(ctx, PVD_LBA + s, block + s * SECTOR)) return;
    static const u8 CD001[5] = {'C', 'D', '0', '0', '1'};
    if (block[0] != 1 || block[6] != 1) return;
    for (int i = 0; i < 5; ++i) if (block[1 + i] != CD001[i]) return;
    img.pvd = true;
    int n = 32;
    while (n > 0 && (block[40 + n - 1] == ' ' || block[40 + n - 1] == 0)) --n;
    for (int i = 0; i < n; ++i) img.volume_id[i] = static_cast<char>(block[40 + i]);
    img.volume_id[n] = 0;
    u64 blocks = le32(block + 80);
    u64 block_size = le16(block + 128);
    if (block_size < 512 || block_size > 65536) block_size = 2048;
    u64 bytes = blocks * block_size;
    u64 capacity = capacity_sectors * SECTOR;
    raise(img, bytes > capacity ? capacity : bytes);
}

inline void scan(ReadFn read, void* ctx, u64 capacity_sectors, Image& img) {
    img = Image{};
    u8 lba0[SECTOR];
    u8 lba1[SECTOR];
    if (capacity_sectors < 2 || !read(ctx, 0, lba0) || !read(ctx, 1, lba1)) return;
    img.readable = true;

    if (lba0[510] == 0x55 && lba0[511] == 0xAA) {
        img.mbr_signature = true;
        for (int p = 0; p < 4; ++p) {
            const u8* e = lba0 + 446 + p * 16;
            u8 type = e[4];
            if (type == 0 || type == 0xEE) continue;
            u64 start = le32(e + 8), len = le32(e + 12);
            if (len == 0) continue;
            ++img.partitions;
            u64 end = (start + len) * SECTOR;
            u64 capacity = capacity_sectors * SECTOR;
            raise(img, end > capacity ? capacity : end);
        }
    }
    scan_gpt(read, ctx, capacity_sectors, img, lba1);
    scan_pvd(read, ctx, capacity_sectors, img);
}

inline bool same_text(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

enum class Verdict : u8 { Match, NotOurs, Unknown };

inline Verdict identify(const Image& img, const u8* want_guid, const char* want_volume_id) {
    if (!img.readable) return Verdict::Unknown;
    if (want_guid && img.gpt) {
        bool same = true;
        for (int i = 0; i < 16; ++i) if (img.disk_guid[i] != want_guid[i]) same = false;
        if (same) return Verdict::Match;
        return Verdict::NotOurs;
    }
    if (want_volume_id && want_volume_id[0] && img.pvd && same_text(img.volume_id, want_volume_id)) return Verdict::Match;
    return Verdict::NotOurs;
}

inline Tail tail(const Image& img, u64 capacity_sectors) {
    Tail t{false, 0, 0, nullptr};
    if (img.used_bytes == 0) { t.why = "no recognisable image on the stick"; return t; }
    if (img.gpt_damaged) { t.why = "the stick's partition table is damaged, so the free space cannot be proven free"; return t; }
    u64 capacity = capacity_sectors * SECTOR;
    u64 start = align_up(img.used_bytes, MIB) + GUARD_BYTES;
    u64 end = capacity / MIB * MIB;
    if (end <= start || end - start < MIN_TAIL_BYTES) {
        t.why = "no free space beyond the image (write it in DD mode to a larger stick)";
        return t;
    }
    t.ok = true;
    t.start_sector = start / SECTOR;
    t.sectors = (end - start) / SECTOR;
    return t;
}

}
