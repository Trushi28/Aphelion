#include <cosmos/stellar.hpp>
#include <cosmos/virtio_blk.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/serial.hpp>

namespace stellar {

constexpr u64 SECTOR_SIZE = 512;
constexpr u64 MAGIC = 0x5AE1157A6111A2C5ull;
constexpr u32 NAME_LEN = 56;

struct PACKED Superblock {
    u64 magic;
    u32 version;
    u32 sector_size;
    u64 total_sectors;
    u64 bitmap_start;
    u64 bitmap_sectors;
    u64 catalog_start;
    u64 catalog_sectors;
    u32 catalog_capacity;
    u32 reserved0;
};

struct PACKED StarEntry {
    u32 type;
    u32 reserved;
    u64 size_bytes;
    u64 first_sector;
    u64 sector_count;
    u8 padding[32];
};
static_assert(sizeof(StarEntry) == 64, "StarEntry must be 64 bytes");

struct PACKED DirEntry {
    u32 in_use;
    u32 star;
    char name[NAME_LEN];
};
static_assert(sizeof(DirEntry) == 64, "DirEntry must be 64 bytes");

static u64 g_hhdm = 0;
static bool g_mounted = false;
static Superblock g_sb{};
static u8* g_bitmap = nullptr;
static StarEntry* g_catalog = nullptr;

static void copy_name(char* dst, const char* src) {
    u32 i = 0;
    for (; i < NAME_LEN - 1 && src[i]; ++i) dst[i] = src[i];
    dst[i] = 0;
}
static bool names_equal(const char* a, const char* b) {
    u32 i = 0;
    for (; i < NAME_LEN; ++i) {
        if (a[i] != b[i]) return false;
        if (a[i] == 0) return true;
    }
    return true;
}

void init(u64 hhdm_offset) { g_hhdm = hhdm_offset; }

static void* alloc_ram(u64 bytes) {
    int order = 0;
    while ((universe::PAGE_SIZE << order) < bytes) ++order;
    u64 phys = universe::alloc(order);
    void* v = reinterpret_cast<void*>(g_hhdm + phys);
    u8* b = static_cast<u8*>(v);
    for (u64 i = 0; i < (universe::PAGE_SIZE << order); ++i) b[i] = 0;
    return v;
}

static void flush_bitmap() {
    for (u64 i = 0; i < g_sb.bitmap_sectors; ++i)
        virtioblk::write_sector(g_sb.bitmap_start + i, g_bitmap + i * SECTOR_SIZE);
}
static void flush_catalog_entry(u32 id) {
    u32 per_sector = static_cast<u32>(SECTOR_SIZE / sizeof(StarEntry));
    u64 sector = g_sb.catalog_start + id / per_sector;
    StarEntry* base = g_catalog + (id / per_sector) * per_sector;
    virtioblk::write_sector(sector, base);
}
static void flush_superblock() {
    u8 buf[SECTOR_SIZE];
    for (auto& b : buf) b = 0;
    __builtin_memcpy(buf, &g_sb, sizeof(g_sb));
    virtioblk::write_sector(0, buf);
}

static u32 alloc_star() {
    for (u32 i = 0; i < g_sb.catalog_capacity; ++i)
        if (g_catalog[i].type == TYPE_FREE) return i;
    return INVALID_STAR;
}

static u64 alloc_sectors(u64 count) {
    u64 data_start = g_sb.catalog_start + g_sb.catalog_sectors;
    u64 run = 0, run_start = 0;
    for (u64 s = data_start; s < g_sb.total_sectors; ++s) {
        bool free_bit = !(g_bitmap[s / 8] & (1u << (s % 8)));
        if (free_bit) {
            if (run == 0) run_start = s;
            ++run;
            if (run == count) {
                for (u64 j = run_start; j < run_start + count; ++j)
                    g_bitmap[j / 8] |= static_cast<u8>(1u << (j % 8));
                flush_bitmap();
                return run_start;
            }
        } else {
            run = 0;
        }
    }
    return 0;
}

static bool add_edge(u32 dir_star, const char* name, u32 target) {
    if (dir_star == INVALID_STAR || g_catalog[dir_star].type != TYPE_CONSTELLATION) return false;
    u8 buf[SECTOR_SIZE];
    if (!virtioblk::read_sector(g_catalog[dir_star].first_sector, buf)) return false;
    DirEntry* entries = reinterpret_cast<DirEntry*>(buf);
    u32 count = static_cast<u32>(SECTOR_SIZE / sizeof(DirEntry));
    for (u32 i = 0; i < count; ++i) {
        if (!entries[i].in_use) {
            entries[i].in_use = 1;
            entries[i].star = target;
            copy_name(entries[i].name, name);
            return virtioblk::write_sector(g_catalog[dir_star].first_sector, buf);
        }
    }
    return false;
}

bool format(u64 total_sectors) {
    g_sb.magic = MAGIC;
    g_sb.version = 1;
    g_sb.sector_size = SECTOR_SIZE;
    g_sb.total_sectors = total_sectors;
    g_sb.bitmap_start = 1;
    g_sb.bitmap_sectors = (total_sectors / 8 + SECTOR_SIZE - 1) / SECTOR_SIZE;
    if (g_sb.bitmap_sectors == 0) g_sb.bitmap_sectors = 1;
    g_sb.catalog_capacity = 256;
    g_sb.catalog_start = g_sb.bitmap_start + g_sb.bitmap_sectors;
    g_sb.catalog_sectors = (g_sb.catalog_capacity * sizeof(StarEntry) + SECTOR_SIZE - 1) / SECTOR_SIZE;

    g_bitmap = static_cast<u8*>(alloc_ram(g_sb.bitmap_sectors * SECTOR_SIZE));
    g_catalog = static_cast<StarEntry*>(alloc_ram(g_sb.catalog_sectors * SECTOR_SIZE));
    g_mounted = true;

    flush_bitmap();
    for (u64 i = 0; i < g_sb.catalog_sectors; ++i)
        virtioblk::write_sector(g_sb.catalog_start + i,
                                  reinterpret_cast<u8*>(g_catalog) + i * SECTOR_SIZE);
    flush_superblock();

    u32 root = create_constellation(INVALID_STAR, "");
    if (root != ROOT_STAR) {
        serial::writeln("[stellar] format: root did not land at star 0");
        return false;
    }
    serial::printf("[stellar] formatted: %lu sectors, %u catalog slots\n",
                    total_sectors, g_sb.catalog_capacity);
    return true;
}

bool mount() {
    u8 buf[SECTOR_SIZE];
    if (!virtioblk::read_sector(0, buf)) return false;
    Superblock sb;
    __builtin_memcpy(&sb, buf, sizeof(sb));
    if (sb.magic != MAGIC) {
        serial::writeln("[stellar] mount: bad magic, not formatted");
        return false;
    }
    g_sb = sb;
    g_bitmap = static_cast<u8*>(alloc_ram(g_sb.bitmap_sectors * SECTOR_SIZE));
    g_catalog = static_cast<StarEntry*>(alloc_ram(g_sb.catalog_sectors * SECTOR_SIZE));
    for (u64 i = 0; i < g_sb.bitmap_sectors; ++i)
        virtioblk::read_sector(g_sb.bitmap_start + i, g_bitmap + i * SECTOR_SIZE);
    for (u64 i = 0; i < g_sb.catalog_sectors; ++i)
        virtioblk::read_sector(g_sb.catalog_start + i,
                                 reinterpret_cast<u8*>(g_catalog) + i * SECTOR_SIZE);
    g_mounted = true;
    serial::printf("[stellar] mounted: %lu sectors, %u catalog slots\n",
                    g_sb.total_sectors, g_sb.catalog_capacity);
    return true;
}

u32 create_constellation(u32 parent, const char* name) {
    if (!g_mounted) return INVALID_STAR;
    u32 id = alloc_star();
    if (id == INVALID_STAR) return INVALID_STAR;
    u64 start = alloc_sectors(1);
    if (start == 0) return INVALID_STAR;

    u8 zero[SECTOR_SIZE];
    for (auto& b : zero) b = 0;
    virtioblk::write_sector(start, zero);

    g_catalog[id].type = TYPE_CONSTELLATION;
    g_catalog[id].size_bytes = 0;
    g_catalog[id].first_sector = start;
    g_catalog[id].sector_count = 1;
    flush_catalog_entry(id);

    if (parent != INVALID_STAR) add_edge(parent, name, id);
    return id;
}

u32 create_file(u32 parent, const char* name, const void* data, u64 size) {
    if (!g_mounted) return INVALID_STAR;
    u32 id = alloc_star();
    if (id == INVALID_STAR) return INVALID_STAR;
    u64 nsec = size == 0 ? 1 : (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    u64 start = alloc_sectors(nsec);
    if (start == 0) return INVALID_STAR;

    const u8* src = static_cast<const u8*>(data);
    u8 buf[SECTOR_SIZE];
    for (u64 i = 0; i < nsec; ++i) {
        u64 done = i * SECTOR_SIZE;
        u64 remaining = done < size ? size - done : 0;
        u64 chunk = remaining < SECTOR_SIZE ? remaining : SECTOR_SIZE;
        for (u64 b = 0; b < SECTOR_SIZE; ++b) buf[b] = (b < chunk) ? src[done + b] : 0;
        if (!virtioblk::write_sector(start + i, buf)) return INVALID_STAR;
    }

    g_catalog[id].type = TYPE_FILE;
    g_catalog[id].size_bytes = size;
    g_catalog[id].first_sector = start;
    g_catalog[id].sector_count = nsec;
    flush_catalog_entry(id);

    if (parent != INVALID_STAR) add_edge(parent, name, id);
    return id;
}

u64 read_file(u32 star, void* buf, u64 max_size) {
    if (!g_mounted || star >= g_sb.catalog_capacity) return 0;
    if (g_catalog[star].type != TYPE_FILE) return 0;
    u64 size = g_catalog[star].size_bytes;
    u64 to_read = size < max_size ? size : max_size;
    u8* dst = static_cast<u8*>(buf);
    u8 sector_buf[SECTOR_SIZE];
    u64 read_so_far = 0;
    for (u64 i = 0; i < g_catalog[star].sector_count && read_so_far < to_read; ++i) {
        if (!virtioblk::read_sector(g_catalog[star].first_sector + i, sector_buf)) break;
        u64 chunk = to_read - read_so_far;
        if (chunk > SECTOR_SIZE) chunk = SECTOR_SIZE;
        for (u64 b = 0; b < chunk; ++b) dst[read_so_far + b] = sector_buf[b];
        read_so_far += chunk;
    }
    return read_so_far;
}

u32 find(u32 dir_star, const char* name) {
    if (!g_mounted || dir_star >= g_sb.catalog_capacity) return INVALID_STAR;
    if (g_catalog[dir_star].type != TYPE_CONSTELLATION) return INVALID_STAR;
    u8 buf[SECTOR_SIZE];
    if (!virtioblk::read_sector(g_catalog[dir_star].first_sector, buf)) return INVALID_STAR;
    DirEntry* entries = reinterpret_cast<DirEntry*>(buf);
    u32 count = static_cast<u32>(SECTOR_SIZE / sizeof(DirEntry));
    for (u32 i = 0; i < count; ++i)
        if (entries[i].in_use && names_equal(entries[i].name, name)) return entries[i].star;
    return INVALID_STAR;
}

void list(u32 dir_star, ListCallback cb, void* ctx) {
    if (!g_mounted || dir_star >= g_sb.catalog_capacity) return;
    if (g_catalog[dir_star].type != TYPE_CONSTELLATION) return;
    u8 buf[SECTOR_SIZE];
    if (!virtioblk::read_sector(g_catalog[dir_star].first_sector, buf)) return;
    DirEntry* entries = reinterpret_cast<DirEntry*>(buf);
    u32 count = static_cast<u32>(SECTOR_SIZE / sizeof(DirEntry));
    for (u32 i = 0; i < count; ++i)
        if (entries[i].in_use)
            cb(entries[i].name, entries[i].star, g_catalog[entries[i].star].type, ctx);
}

}
