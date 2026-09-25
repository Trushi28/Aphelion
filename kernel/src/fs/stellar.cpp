#include <cosmos/stellar.hpp>
#include <cosmos/blockdev.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/serial.hpp>

namespace stellar {

constexpr u64 SECTOR_SIZE = 512;
constexpr u64 MAGIC = 0x5AE1157A6111A2C5ull;
constexpr u32 VERSION = 2;
constexpr u32 NAME_LEN = 52;
constexpr u32 LEAF_MAX = 6;
constexpr u32 INTERNAL_MAX = 30;

struct PACKED Superblock {
    u64 magic;
    u32 version;
    u32 sector_size;
    u64 total_sectors;
    u64 bitmap_start;
    u64 bitmap_sectors;
    u64 catalog_root;
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
    u8 padding[32];
};
static_assert(sizeof(StarEntry) == 64, "StarEntry must be 64 bytes");

struct PACKED DirEntry {
    u32 in_use;
    u64 star;
    char name[NAME_LEN];
};
static_assert(sizeof(DirEntry) == 64, "DirEntry must be 64 bytes");

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

static void catalog_insert(u64 key, const StarEntry& value) {
    u64 promoted_key = 0, new_right = 0;
    if (!btree_insert(g_sb.catalog_root, key, value, &promoted_key, &new_right)) return;

    u8 buf[SECTOR_SIZE];
    for (auto& b : buf) b = 0;
    auto* root = reinterpret_cast<InternalNode*>(buf);
    root->is_leaf = 0;
    root->count = 1;
    root->key[0] = promoted_key;
    root->child[0] = g_sb.catalog_root;
    root->child[1] = new_right;

    u64 new_root_sector = alloc_node_sector();
    blockdev::write_sector(new_root_sector, buf);

    g_sb.catalog_root = new_root_sector;
    flush_superblock();
}

static bool catalog_find(u64 key, StarEntry* out) {
    return btree_search(g_sb.catalog_root, key, out);
}

static bool add_edge(u64 dir_star, const char* name, u64 target) {
    StarEntry e;
    if (!catalog_find(dir_star, &e) || e.type != TYPE_CONSTELLATION) return false;
    u8 buf[SECTOR_SIZE];
    if (!blockdev::read_sector(e.first_sector, buf)) return false;
    DirEntry* entries = reinterpret_cast<DirEntry*>(buf);
    u32 count = static_cast<u32>(SECTOR_SIZE / sizeof(DirEntry));
    for (u32 i = 0; i < count; ++i) {
        if (!entries[i].in_use) {
            entries[i].in_use = 1;
            entries[i].star = target;
            copy_name(entries[i].name, name);
            return blockdev::write_sector(e.first_sector, buf);
        }
    }
    return false;
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

    g_bitmap = static_cast<u8*>(alloc_ram(g_sb.bitmap_sectors * SECTOR_SIZE));
    g_mounted = true;

    u64 root_node_sector = alloc_sectors(1);
    if (root_node_sector == 0) return false;
    u8 buf[SECTOR_SIZE];
    for (auto& b : buf) b = 0;
    reinterpret_cast<LeafNode*>(buf)->is_leaf = 1;
    blockdev::write_sector(root_node_sector, buf);
    g_sb.catalog_root = root_node_sector;

    flush_bitmap();
    flush_superblock();

    u64 root = create_constellation(INVALID_STAR, "");
    if (root != ROOT_STAR) {
        serial::writeln("[stellar] format: root did not land at star 0");
        return false;
    }
    serial::printf("[stellar] formatted: %lu sectors, B+tree catalog (leaf fanout %u, internal fanout %u)\n",
                    total_sectors, LEAF_MAX, INTERNAL_MAX);
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

    const u8* src = static_cast<const u8*>(data);
    u8 buf[SECTOR_SIZE];
    for (u64 i = 0; i < nsec; ++i) {
        u64 done = i * SECTOR_SIZE;
        u64 remaining = done < size ? size - done : 0;
        u64 chunk = remaining < SECTOR_SIZE ? remaining : SECTOR_SIZE;
        for (u64 b = 0; b < SECTOR_SIZE; ++b) buf[b] = (b < chunk) ? src[done + b] : 0;
        if (!blockdev::write_sector(start + i, buf)) return INVALID_STAR;
    }

    StarEntry entry{};
    entry.type = TYPE_FILE;
    entry.size_bytes = size;
    entry.first_sector = start;
    entry.sector_count = nsec;
    catalog_insert(id, entry);

    if (parent != INVALID_STAR) add_edge(parent, name, id);
    return id;
}

u64 read_file(u64 star, void* buf, u64 max_size) {
    if (!g_mounted) return 0;
    StarEntry e;
    if (!catalog_find(star, &e) || e.type != TYPE_FILE) return 0;
    u64 size = e.size_bytes;
    u64 to_read = size < max_size ? size : max_size;
    u8* dst = static_cast<u8*>(buf);
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
    u8 buf[SECTOR_SIZE];
    if (!blockdev::read_sector(e.first_sector, buf)) return INVALID_STAR;
    DirEntry* entries = reinterpret_cast<DirEntry*>(buf);
    u32 count = static_cast<u32>(SECTOR_SIZE / sizeof(DirEntry));
    for (u32 i = 0; i < count; ++i)
        if (entries[i].in_use && names_equal(entries[i].name, name)) return entries[i].star;
    return INVALID_STAR;
}

void list(u64 dir_star, ListCallback cb, void* ctx) {
    if (!g_mounted) return;
    StarEntry e;
    if (!catalog_find(dir_star, &e) || e.type != TYPE_CONSTELLATION) return;
    u8 buf[SECTOR_SIZE];
    if (!blockdev::read_sector(e.first_sector, buf)) return;
    DirEntry* entries = reinterpret_cast<DirEntry*>(buf);
    u32 count = static_cast<u32>(SECTOR_SIZE / sizeof(DirEntry));
    for (u32 i = 0; i < count; ++i) {
        if (!entries[i].in_use) continue;
        StarEntry child;
        u32 type = catalog_find(entries[i].star, &child) ? child.type : TYPE_FREE;
        cb(entries[i].name, entries[i].star, type, ctx);
    }
}

}
