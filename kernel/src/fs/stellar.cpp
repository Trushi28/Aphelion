#include <cosmos/stellar.hpp>
#include <cosmos/blockdev.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/serial.hpp>

namespace stellar {

constexpr u64 SECTOR_SIZE = 512;
constexpr u64 MAGIC = 0x5AE1157A6111A2C5ull;
constexpr u32 VERSION = 3;
constexpr u32 NAME_LEN = 52;
constexpr u32 LEAF_MAX = 6;
constexpr u32 INTERNAL_MAX = 30;
constexpr u32 SNAPSHOT_MAX_DEPTH = 8;

struct PACKED Superblock {
    u64 magic;
    u32 version;
    u32 sector_size;
    u64 total_sectors;
    u64 bitmap_start;
    u64 bitmap_sectors;
    u64 catalog_root;
    u64 extent_root;
    u64 next_star_id;
    u32 reserved0;
    u32 reserved1;
};

struct PACKED StarEntry {
    u32 type;
    u32 reserved;
    u64 size_bytes;
    u64 first_sector;
    u64 sector_count;
    u32 checksum;
    u8 padding[28];
};
static_assert(sizeof(StarEntry) == 64, "StarEntry must be 64 bytes");

struct PACKED DirEntry {
    u32 in_use;
    u64 star;
    char name[NAME_LEN];
};
static_assert(sizeof(DirEntry) == 64, "DirEntry must be 64 bytes");

constexpr u32 DIR_ENTRIES_PER_SECTOR = (SECTOR_SIZE - 8) / sizeof(DirEntry);

struct PACKED DirSector {
    u64 next_sector;
    DirEntry entries[DIR_ENTRIES_PER_SECTOR];
    u8 padding[SECTOR_SIZE - 8 - DIR_ENTRIES_PER_SECTOR * sizeof(DirEntry)];
};
static_assert(sizeof(DirSector) == SECTOR_SIZE, "DirSector must fill one sector");

struct PACKED LeafEntry {
    u64 star_id;
    StarEntry entry;
};
static_assert(sizeof(LeafEntry) == 72, "LeafEntry must be 72 bytes");

struct PACKED LeafNode {
    u8 is_leaf;
    u8 reserved0[3];
    u32 count;
    u64 next_leaf;
    LeafEntry entries[LEAF_MAX];
    u8 padding[SECTOR_SIZE - 16 - LEAF_MAX * sizeof(LeafEntry)];
};
static_assert(sizeof(LeafNode) == SECTOR_SIZE, "LeafNode must fill one sector");

struct PACKED InternalNode {
    u8 is_leaf;
    u8 reserved0[3];
    u32 count;
    u64 reserved1;
    u64 key[INTERNAL_MAX];
    u64 child[INTERNAL_MAX + 1];
    u8 padding[SECTOR_SIZE - 16 - INTERNAL_MAX * 8 - (INTERNAL_MAX + 1) * 8];
};
static_assert(sizeof(InternalNode) == SECTOR_SIZE, "InternalNode must fill one sector");

static u64 g_hhdm = 0;
static bool g_mounted = false;
static Superblock g_sb{};
static u8* g_bitmap = nullptr;

static u32 g_crc32_table[256];
static bool g_crc32_ready = false;

static void crc32_init() {
    for (u32 i = 0; i < 256; ++i) {
        u32 c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc32_table[i] = c;
    }
    g_crc32_ready = true;
}
static u32 crc32_update(u32 crc, const u8* data, u64 len) {
    if (!g_crc32_ready) crc32_init();
    for (u64 i = 0; i < len; ++i) crc = g_crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}
static u32 crc32_full(const u8* data, u64 len) {
    return crc32_update(0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}

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
        blockdev::write_sector(g_sb.bitmap_start + i, g_bitmap + i * SECTOR_SIZE);
}
static void flush_superblock() {
    u8 buf[SECTOR_SIZE];
    for (auto& b : buf) b = 0;
    __builtin_memcpy(buf, &g_sb, sizeof(g_sb));
    blockdev::write_sector(0, buf);
}

static u64 alloc_sectors(u64 count) {
    u64 data_start = g_sb.bitmap_start + g_sb.bitmap_sectors;
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

static void free_sectors(u64 first, u64 count) {
    for (u64 j = first; j < first + count; ++j)
        g_bitmap[j / 8] &= static_cast<u8>(~(1u << (j % 8)));
    flush_bitmap();
}

static u64 alloc_node_sector() { return alloc_sectors(1); }

static u64 alloc_star_id() {
    u64 id = g_sb.next_star_id;
    ++g_sb.next_star_id;
    flush_superblock();
    return id;
}

static bool btree_search(u64 node_sector, u64 key, StarEntry* out) {
    u8 buf[SECTOR_SIZE];
    if (!blockdev::read_sector(node_sector, buf)) return false;
    if (buf[0]) {
        auto* leaf = reinterpret_cast<LeafNode*>(buf);
        for (u32 i = 0; i < leaf->count; ++i) {
            if (leaf->entries[i].star_id == key) {
                *out = leaf->entries[i].entry;
                return true;
            }
        }
        return false;
    }
    auto* node = reinterpret_cast<InternalNode*>(buf);
    u32 i = 0;
    while (i < node->count && key >= node->key[i]) ++i;
    return btree_search(node->child[i], key, out);
}

static bool btree_update(u64 node_sector, u64 key, const StarEntry& value) {
    u8 buf[SECTOR_SIZE];
    if (!blockdev::read_sector(node_sector, buf)) return false;
    if (buf[0]) {
        auto* leaf = reinterpret_cast<LeafNode*>(buf);
        for (u32 i = 0; i < leaf->count; ++i) {
            if (leaf->entries[i].star_id == key) {
                leaf->entries[i].entry = value;
                return blockdev::write_sector(node_sector, buf);
            }
        }
        return false;
    }
    auto* node = reinterpret_cast<InternalNode*>(buf);
    u32 i = 0;
    while (i < node->count && key >= node->key[i]) ++i;
    return btree_update(node->child[i], key, value);
}

static bool btree_insert(u64 node_sector, u64 key, const StarEntry& value,
                          u64* promoted_key, u64* new_right_sector) {
    u8 buf[SECTOR_SIZE];
    blockdev::read_sector(node_sector, buf);

    if (buf[0]) {
        auto* leaf = reinterpret_cast<LeafNode*>(buf);
        LeafEntry tmp[LEAF_MAX + 1];
        u32 pos = 0;
        while (pos < leaf->count && leaf->entries[pos].star_id < key) ++pos;
        for (u32 i = 0; i < pos; ++i) tmp[i] = leaf->entries[i];
        tmp[pos].star_id = key;
        tmp[pos].entry = value;
        for (u32 i = pos; i < leaf->count; ++i) tmp[i + 1] = leaf->entries[i];
        u32 total = leaf->count + 1;

        if (total <= LEAF_MAX) {
            leaf->count = total;
            for (u32 i = 0; i < total; ++i) leaf->entries[i] = tmp[i];
            blockdev::write_sector(node_sector, buf);
            return false;
        }

        u32 left_count = total / 2;
        u32 right_count = total - left_count;

        leaf->count = left_count;
        for (u32 i = 0; i < left_count; ++i) leaf->entries[i] = tmp[i];

        u8 rbuf[SECTOR_SIZE];
        for (auto& b : rbuf) b = 0;
        auto* rleaf = reinterpret_cast<LeafNode*>(rbuf);
        rleaf->is_leaf = 1;
        rleaf->count = right_count;
        for (u32 i = 0; i < right_count; ++i) rleaf->entries[i] = tmp[left_count + i];
        rleaf->next_leaf = leaf->next_leaf;

        u64 rsector = alloc_node_sector();
        blockdev::write_sector(rsector, rbuf);

        leaf->next_leaf = rsector;
        blockdev::write_sector(node_sector, buf);

        *promoted_key = tmp[left_count].star_id;
        *new_right_sector = rsector;
        return true;
    }

    auto* node = reinterpret_cast<InternalNode*>(buf);
    u32 i = 0;
    while (i < node->count && key >= node->key[i]) ++i;

    u64 child_promoted = 0, child_right = 0;
    if (!btree_insert(node->child[i], key, value, &child_promoted, &child_right)) return false;

    u64 tmp_key[INTERNAL_MAX + 1];
    u64 tmp_child[INTERNAL_MAX + 2];
    for (u32 j = 0; j < i; ++j) tmp_key[j] = node->key[j];
    for (u32 j = 0; j <= i; ++j) tmp_child[j] = node->child[j];
    tmp_key[i] = child_promoted;
    tmp_child[i + 1] = child_right;
    for (u32 j = i; j < node->count; ++j) tmp_key[j + 1] = node->key[j];
    for (u32 j = i + 1; j <= node->count; ++j) tmp_child[j + 1] = node->child[j];
    u32 total = node->count + 1;

    if (total <= INTERNAL_MAX) {
        node->count = total;
        for (u32 j = 0; j < total; ++j) node->key[j] = tmp_key[j];
        for (u32 j = 0; j <= total; ++j) node->child[j] = tmp_child[j];
        blockdev::write_sector(node_sector, buf);
        return false;
    }

    u32 mid = total / 2;
    u64 up_key = tmp_key[mid];

    node->count = mid;
    for (u32 j = 0; j < mid; ++j) node->key[j] = tmp_key[j];
    for (u32 j = 0; j <= mid; ++j) node->child[j] = tmp_child[j];
    blockdev::write_sector(node_sector, buf);

    u8 rbuf[SECTOR_SIZE];
    for (auto& b : rbuf) b = 0;
    auto* rnode = reinterpret_cast<InternalNode*>(rbuf);
    rnode->is_leaf = 0;
    u32 right_count = total - mid - 1;
    rnode->count = right_count;
    for (u32 j = 0; j < right_count; ++j) rnode->key[j] = tmp_key[mid + 1 + j];
    for (u32 j = 0; j <= right_count; ++j) rnode->child[j] = tmp_child[mid + 1 + j];

    u64 rsector = alloc_node_sector();
    blockdev::write_sector(rsector, rbuf);

    *promoted_key = up_key;
    *new_right_sector = rsector;
    return true;
}

static void tree_insert(u64* root, u64 key, const StarEntry& value) {
    u64 promoted_key = 0, new_right = 0;
    if (!btree_insert(*root, key, value, &promoted_key, &new_right)) return;

    u8 buf[SECTOR_SIZE];
    for (auto& b : buf) b = 0;
    auto* root_node = reinterpret_cast<InternalNode*>(buf);
    root_node->is_leaf = 0;
    root_node->count = 1;
    root_node->key[0] = promoted_key;
    root_node->child[0] = *root;
    root_node->child[1] = new_right;

    u64 new_root_sector = alloc_node_sector();
    blockdev::write_sector(new_root_sector, buf);

    *root = new_root_sector;
    flush_superblock();
}

static void catalog_insert(u64 key, const StarEntry& value) { tree_insert(&g_sb.catalog_root, key, value); }
static bool catalog_find(u64 key, StarEntry* out) { return btree_search(g_sb.catalog_root, key, out); }
static bool catalog_update(u64 key, const StarEntry& value) { return btree_update(g_sb.catalog_root, key, value); }

static u32 extent_refcount(u64 first_sector) {
    StarEntry tmp;
    if (!btree_search(g_sb.extent_root, first_sector, &tmp)) return 1;
    return static_cast<u32>(tmp.size_bytes);
}
static void extent_set_refcount(u64 first_sector, u32 rc) {
    StarEntry tmp{};
    tmp.size_bytes = rc;
    if (!btree_update(g_sb.extent_root, first_sector, tmp))
        tree_insert(&g_sb.extent_root, first_sector, tmp);
}
static void release_extent(u64 first_sector, u64 sector_count) {
    u32 rc = extent_refcount(first_sector);
    if (rc <= 1) {
        free_sectors(first_sector, sector_count);
        return;
    }
    extent_set_refcount(first_sector, rc - 1);
}

static bool write_extent(u64 start, u64 nsec, const void* data, u64 size) {
    if (size != 0 && size == nsec * SECTOR_SIZE && blockdev::write_sectors(start, nsec, data)) return true;
    const u8* src = static_cast<const u8*>(data);
    u8 buf[SECTOR_SIZE];
    for (u64 i = 0; i < nsec; ++i) {
        u64 done = i * SECTOR_SIZE;
        u64 remaining = done < size ? size - done : 0;
        u64 chunk = remaining < SECTOR_SIZE ? remaining : SECTOR_SIZE;
        for (u64 b = 0; b < SECTOR_SIZE; ++b) buf[b] = (b < chunk) ? src[done + b] : 0;
        if (!blockdev::write_sector(start + i, buf)) return false;
    }
    return true;
}

static bool add_edge(u64 dir_star, const char* name, u64 target) {
    StarEntry e;
    if (!catalog_find(dir_star, &e) || e.type != TYPE_CONSTELLATION) return false;

    u64 sector = e.first_sector;
    u8 buf[SECTOR_SIZE];
    for (;;) {
        if (!blockdev::read_sector(sector, buf)) return false;
        auto* ds = reinterpret_cast<DirSector*>(buf);
        for (u32 i = 0; i < DIR_ENTRIES_PER_SECTOR; ++i) {
            if (!ds->entries[i].in_use) {
                ds->entries[i].in_use = 1;
                ds->entries[i].star = target;
                copy_name(ds->entries[i].name, name);
                return blockdev::write_sector(sector, buf);
            }
        }
        if (ds->next_sector != 0) {
            sector = ds->next_sector;
            continue;
        }

        u64 new_sector = alloc_sectors(1);
        if (new_sector == 0) return false;
        u8 zero[SECTOR_SIZE];
        for (auto& b : zero) b = 0;
        blockdev::write_sector(new_sector, zero);

        ds->next_sector = new_sector;
        blockdev::write_sector(sector, buf);

        e.sector_count += 1;
        catalog_update(dir_star, e);

        sector = new_sector;
    }
}

bool format(u64 total_sectors) {
    g_sb.magic = MAGIC;
    g_sb.version = VERSION;
    g_sb.sector_size = SECTOR_SIZE;
    g_sb.total_sectors = total_sectors;
    g_sb.bitmap_start = 1;
    g_sb.bitmap_sectors = (total_sectors / 8 + SECTOR_SIZE - 1) / SECTOR_SIZE;
    if (g_sb.bitmap_sectors == 0) g_sb.bitmap_sectors = 1;
    g_sb.next_star_id = 0;
    g_sb.catalog_root = 0;
    g_sb.extent_root = 0;

    g_bitmap = static_cast<u8*>(alloc_ram(g_sb.bitmap_sectors * SECTOR_SIZE));
    g_mounted = true;

    u64 catalog_root_sector = alloc_sectors(1);
    u64 extent_root_sector = alloc_sectors(1);
    if (catalog_root_sector == 0 || extent_root_sector == 0) return false;
    u8 buf[SECTOR_SIZE];
    for (auto& b : buf) b = 0;
    reinterpret_cast<LeafNode*>(buf)->is_leaf = 1;
    blockdev::write_sector(catalog_root_sector, buf);
    blockdev::write_sector(extent_root_sector, buf);
    g_sb.catalog_root = catalog_root_sector;
    g_sb.extent_root = extent_root_sector;

    flush_bitmap();
    flush_superblock();

    u64 root = create_constellation(INVALID_STAR, "");
    if (root != ROOT_STAR) {
        serial::writeln("[stellar] format: root did not land at star 0");
        return false;
    }
    serial::printf("[stellar] formatted: %lu sectors, B+tree catalog (leaf fanout %u, internal fanout %u), directory fanout %u/sector\n",
                    total_sectors, LEAF_MAX, INTERNAL_MAX, DIR_ENTRIES_PER_SECTOR);
    return true;
}

bool mount() {
    u8 buf[SECTOR_SIZE];
    if (!blockdev::read_sector(0, buf)) return false;
    Superblock sb;
    __builtin_memcpy(&sb, buf, sizeof(sb));
    if (sb.magic != MAGIC || sb.version != VERSION) {
        serial::writeln("[stellar] mount: bad magic or version, not formatted");
        return false;
    }
    g_sb = sb;
    g_bitmap = static_cast<u8*>(alloc_ram(g_sb.bitmap_sectors * SECTOR_SIZE));
    for (u64 i = 0; i < g_sb.bitmap_sectors; ++i)
        blockdev::read_sector(g_sb.bitmap_start + i, g_bitmap + i * SECTOR_SIZE);
    g_mounted = true;
    serial::printf("[stellar] mounted: %lu sectors, B+tree root at sector %lu, next star id %lu\n",
                    g_sb.total_sectors, g_sb.catalog_root, g_sb.next_star_id);
    return true;
}

u64 create_constellation(u64 parent, const char* name) {
    if (!g_mounted) return INVALID_STAR;
    u64 id = alloc_star_id();
    u64 start = alloc_sectors(1);
    if (start == 0) return INVALID_STAR;

    u8 zero[SECTOR_SIZE];
    for (auto& b : zero) b = 0;
    blockdev::write_sector(start, zero);

    StarEntry entry{};
    entry.type = TYPE_CONSTELLATION;
    entry.size_bytes = 0;
    entry.first_sector = start;
    entry.sector_count = 1;
    catalog_insert(id, entry);

    if (parent != INVALID_STAR) add_edge(parent, name, id);
    return id;
}

u64 create_file(u64 parent, const char* name, const void* data, u64 size) {
    if (!g_mounted) return INVALID_STAR;
    u64 id = alloc_star_id();
    u64 nsec = size == 0 ? 1 : (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    u64 start = alloc_sectors(nsec);
    if (start == 0) return INVALID_STAR;
    if (!write_extent(start, nsec, data, size)) return INVALID_STAR;

    StarEntry entry{};
    entry.type = TYPE_FILE;
    entry.size_bytes = size;
    entry.first_sector = start;
    entry.sector_count = nsec;
    entry.checksum = crc32_full(static_cast<const u8*>(data), size);
    catalog_insert(id, entry);

    if (parent != INVALID_STAR) add_edge(parent, name, id);
    return id;
}

u64 write_file(u64 star, const void* data, u64 size) {
    if (!g_mounted) return INVALID_STAR;
    StarEntry old_e;
    if (!catalog_find(star, &old_e) || old_e.type != TYPE_FILE) return INVALID_STAR;

    u64 nsec = size == 0 ? 1 : (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    u64 start = alloc_sectors(nsec);
    if (start == 0) return INVALID_STAR;
    if (!write_extent(start, nsec, data, size)) return INVALID_STAR;

    StarEntry new_e{};
    new_e.type = TYPE_FILE;
    new_e.size_bytes = size;
    new_e.first_sector = start;
    new_e.sector_count = nsec;
    new_e.checksum = crc32_full(static_cast<const u8*>(data), size);

    if (!catalog_update(star, new_e)) return INVALID_STAR;
    release_extent(old_e.first_sector, old_e.sector_count);
    return star;
}

static u64 snapshot_file(const StarEntry& e) {
    u32 rc = extent_refcount(e.first_sector);
    extent_set_refcount(e.first_sector, rc + 1);

    u64 new_id = alloc_star_id();
    catalog_insert(new_id, e);
    return new_id;
}

static u64 snapshot_tree(u64 star, u32 depth) {
    if (depth > SNAPSHOT_MAX_DEPTH) {
        serial::writeln("[stellar] snapshot: namespace too deep or cyclic");
        return INVALID_STAR;
    }
    StarEntry e;
    if (!catalog_find(star, &e)) return INVALID_STAR;
    if (e.type == TYPE_FILE) return snapshot_file(e);
    if (e.type != TYPE_CONSTELLATION) return INVALID_STAR;

    u64 new_dir = create_constellation(INVALID_STAR, "");
    if (new_dir == INVALID_STAR) return INVALID_STAR;

    u64 sector = e.first_sector;
    u8 buf[SECTOR_SIZE];
    while (sector != 0) {
        if (!blockdev::read_sector(sector, buf)) return INVALID_STAR;
        auto* ds = reinterpret_cast<DirSector*>(buf);
        for (u32 i = 0; i < DIR_ENTRIES_PER_SECTOR; ++i) {
            if (!ds->entries[i].in_use) continue;
            u64 child = snapshot_tree(ds->entries[i].star, depth + 1);
            if (child == INVALID_STAR) return INVALID_STAR;
            if (!add_edge(new_dir, ds->entries[i].name, child)) return INVALID_STAR;
        }
        sector = ds->next_sector;
    }
    return new_dir;
}

u64 snapshot(u64 star) {
    if (!g_mounted) return INVALID_STAR;
    return snapshot_tree(star, 0);
}

bool link(u64 dir_star, const char* name, u64 target) {
    if (!g_mounted) return false;
    StarEntry e;
    if (!catalog_find(target, &e)) return false;
    return add_edge(dir_star, name, target);
}

bool verify_file(u64 star) {
    if (!g_mounted) return false;
    StarEntry e;
    if (!catalog_find(star, &e) || e.type != TYPE_FILE) return false;

    u32 crc = 0xFFFFFFFFu;
    u8 buf[SECTOR_SIZE];
    u64 remaining = e.size_bytes;
    for (u64 i = 0; i < e.sector_count && remaining > 0; ++i) {
        if (!blockdev::read_sector(e.first_sector + i, buf)) return false;
        u64 chunk = remaining < SECTOR_SIZE ? remaining : SECTOR_SIZE;
        crc = crc32_update(crc, buf, chunk);
        remaining -= chunk;
    }
    crc ^= 0xFFFFFFFFu;

    if (crc != e.checksum) {
        serial::printf("[stellar] checksum mismatch on star %lu: stored %x computed %x\n",
                        star, e.checksum, crc);
        return false;
    }
    return true;
}

u64 read_file(u64 star, void* buf, u64 max_size) {
    if (!g_mounted) return 0;
    StarEntry e;
    if (!catalog_find(star, &e) || e.type != TYPE_FILE) return 0;
    u64 size = e.size_bytes;
    u64 to_read = size < max_size ? size : max_size;
    u8* dst = static_cast<u8*>(buf);
    if (e.sector_count > 1 && max_size >= e.sector_count * SECTOR_SIZE &&
        blockdev::read_sectors(e.first_sector, e.sector_count, dst))
        return to_read;
    u8 sector_buf[SECTOR_SIZE];
    u64 read_so_far = 0;
    for (u64 i = 0; i < e.sector_count && read_so_far < to_read; ++i) {
        if (!blockdev::read_sector(e.first_sector + i, sector_buf)) break;
        u64 chunk = to_read - read_so_far;
        if (chunk > SECTOR_SIZE) chunk = SECTOR_SIZE;
        for (u64 b = 0; b < chunk; ++b) dst[read_so_far + b] = sector_buf[b];
        read_so_far += chunk;
    }
    return read_so_far;
}

u64 find(u64 dir_star, const char* name) {
    if (!g_mounted) return INVALID_STAR;
    StarEntry e;
    if (!catalog_find(dir_star, &e) || e.type != TYPE_CONSTELLATION) return INVALID_STAR;
    u64 sector = e.first_sector;
    u8 buf[SECTOR_SIZE];
    while (sector != 0) {
        if (!blockdev::read_sector(sector, buf)) return INVALID_STAR;
        auto* ds = reinterpret_cast<DirSector*>(buf);
        for (u32 i = 0; i < DIR_ENTRIES_PER_SECTOR; ++i)
            if (ds->entries[i].in_use && names_equal(ds->entries[i].name, name)) return ds->entries[i].star;
        sector = ds->next_sector;
    }
    return INVALID_STAR;
}

void list(u64 dir_star, ListCallback cb, void* ctx) {
    if (!g_mounted) return;
    StarEntry e;
    if (!catalog_find(dir_star, &e) || e.type != TYPE_CONSTELLATION) return;
    u64 sector = e.first_sector;
    u8 buf[SECTOR_SIZE];
    while (sector != 0) {
        if (!blockdev::read_sector(sector, buf)) return;
        auto* ds = reinterpret_cast<DirSector*>(buf);
        for (u32 i = 0; i < DIR_ENTRIES_PER_SECTOR; ++i) {
            if (!ds->entries[i].in_use) continue;
            StarEntry child;
            u32 type = catalog_find(ds->entries[i].star, &child) ? child.type : TYPE_FREE;
            cb(ds->entries[i].name, ds->entries[i].star, type, ctx);
        }
        sector = ds->next_sector;
    }
}

}
