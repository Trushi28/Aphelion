#include <cosmos/stellar.hpp>
#include <cosmos/blockdev.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/serial.hpp>

namespace stellar {

constexpr u64 SECTOR_SIZE = 512;
constexpr u64 MAGIC = 0x5AE1157A6111A2C5ull;
constexpr u32 VERSION = 5;
constexpr u32 NAME_LEN = 52;
constexpr u32 LEAF_MAX = 10;
constexpr u32 INTERNAL_MAX = 30;
constexpr u32 CACHE_SLOTS = 256;

struct PACKED SnapRec {
    u64 catalog_root;
    u64 epoch;
};

struct PACKED Superblock {
    u64 magic;
    u32 version;
    u32 sector_size;
    u64 total_sectors;
    u64 bitmap_start;
    u64 bitmap_sectors;
    u64 catalog_root;
    u64 next_star_id;
    u64 epoch;
    u32 snap_count;
    u32 reserved;
    SnapRec snaps[SNAPSHOT_MAX];
};
static_assert(sizeof(Superblock) <= SECTOR_SIZE, "superblock must fit one sector");

struct PACKED StarEntry {
    u32 type;
    u32 checksum;
    u64 size_bytes;
    u64 first_sector;
    u64 gen;
    u32 sector_count;
    u32 nlink;
};
static_assert(sizeof(StarEntry) == 40, "StarEntry must be 40 bytes");

struct PACKED DirEntry {
    u32 in_use;
    u64 star;
    char name[NAME_LEN];
};
static_assert(sizeof(DirEntry) == 64, "DirEntry must be 64 bytes");

constexpr u32 DIR_ENTRIES_PER_SECTOR = (SECTOR_SIZE - 16) / sizeof(DirEntry);

struct PACKED DirSector {
    u64 next_sector;
    u64 gen;
    DirEntry entries[DIR_ENTRIES_PER_SECTOR];
    u8 padding[SECTOR_SIZE - 16 - DIR_ENTRIES_PER_SECTOR * sizeof(DirEntry)];
};
static_assert(sizeof(DirSector) == SECTOR_SIZE, "DirSector must fill one sector");

struct PACKED NodeHeader {
    u8 is_leaf;
    u8 reserved0[3];
    u32 count;
    u64 gen;
};

struct PACKED LeafEntry {
    u64 star_id;
    StarEntry entry;
};
static_assert(sizeof(LeafEntry) == 48, "LeafEntry must be 48 bytes");

struct PACKED LeafNode {
    u8 is_leaf;
    u8 reserved0[3];
    u32 count;
    u64 gen;
    LeafEntry entries[LEAF_MAX];
    u8 padding[SECTOR_SIZE - 16 - LEAF_MAX * sizeof(LeafEntry)];
};
static_assert(sizeof(LeafNode) == SECTOR_SIZE, "LeafNode must fill one sector");

struct PACKED InternalNode {
    u8 is_leaf;
    u8 reserved0[3];
    u32 count;
    u64 gen;
    u64 key[INTERNAL_MAX];
    u64 child[INTERNAL_MAX + 1];
    u8 padding[SECTOR_SIZE - 16 - INTERNAL_MAX * 8 - (INTERNAL_MAX + 1) * 8];
};
static_assert(sizeof(InternalNode) == SECTOR_SIZE, "InternalNode must fill one sector");

struct CacheSlot {
    u64 sector;
    bool valid;
    u8 data[SECTOR_SIZE];
};

static u64 g_hhdm = 0;
static bool g_mounted = false;
static Superblock g_sb{};
static u8* g_bitmap = nullptr;
static u8* g_bm_dirty = nullptr;
static u64 g_bm_lo = ~0ull, g_bm_hi = 0;
static bool g_sb_dirty = false;
static u64 g_alloc_hint = 0;

static CacheSlot g_cache[CACHE_SLOTS];
static u64 g_io_reads = 0, g_io_writes = 0, g_cache_hits = 0;

static u32 g_crc32_table[8][256];
static bool g_crc32_ready = false;

static void crc32_init() {
    for (u32 i = 0; i < 256; ++i) {
        u32 c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc32_table[0][i] = c;
    }
    for (u32 i = 0; i < 256; ++i) {
        for (u32 k = 1; k < 8; ++k) {
            u32 prev = g_crc32_table[k - 1][i];
            g_crc32_table[k][i] = (prev >> 8) ^ g_crc32_table[0][prev & 0xFF];
        }
    }
    g_crc32_ready = true;
}
static u32 crc32_update(u32 crc, const u8* data, u64 len) {
    if (!g_crc32_ready) crc32_init();
    while (len >= 8) {
        u32 a = crc ^ (static_cast<u32>(data[0]) | (static_cast<u32>(data[1]) << 8) |
                       (static_cast<u32>(data[2]) << 16) | (static_cast<u32>(data[3]) << 24));
        u32 b = static_cast<u32>(data[4]) | (static_cast<u32>(data[5]) << 8) |
                (static_cast<u32>(data[6]) << 16) | (static_cast<u32>(data[7]) << 24);
        crc = g_crc32_table[7][a & 0xFF] ^ g_crc32_table[6][(a >> 8) & 0xFF] ^
              g_crc32_table[5][(a >> 16) & 0xFF] ^ g_crc32_table[4][a >> 24] ^
              g_crc32_table[3][b & 0xFF] ^ g_crc32_table[2][(b >> 8) & 0xFF] ^
              g_crc32_table[1][(b >> 16) & 0xFF] ^ g_crc32_table[0][b >> 24];
        data += 8;
        len -= 8;
    }
    for (u64 i = 0; i < len; ++i) crc = g_crc32_table[0][(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}
static u32 crc32_full(const u8* data, u64 len) {
    return crc32_update(0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}

u32 crc32(const void* data, u64 len) {
    return crc32_full(static_cast<const u8*>(data), len);
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

IoStats io_stats() { return { g_io_reads, g_io_writes, g_cache_hits }; }
u32 snapshot_count() { return g_sb.snap_count; }

static void* alloc_ram(u64 bytes) {
    int order = 0;
    while ((universe::PAGE_SIZE << order) < bytes) ++order;
    u64 phys = universe::alloc(order);
    void* v = reinterpret_cast<void*>(g_hhdm + phys);
    __builtin_memset(v, 0, universe::PAGE_SIZE << order);
    return v;
}

static bool raw_wr(u64 sector, const void* buf) {
    ++g_io_writes;
    return blockdev::write_sector(sector, buf);
}

static bool rd(u64 sector, void* out) {
    CacheSlot& c = g_cache[sector % CACHE_SLOTS];
    if (c.valid && c.sector == sector) {
        ++g_cache_hits;
        __builtin_memcpy(out, c.data, SECTOR_SIZE);
        return true;
    }
    ++g_io_reads;
    if (!blockdev::read_sector(sector, c.data)) { c.valid = false; return false; }
    c.sector = sector;
    c.valid = true;
    __builtin_memcpy(out, c.data, SECTOR_SIZE);
    return true;
}

static bool wr(u64 sector, const void* in) {
    CacheSlot& c = g_cache[sector % CACHE_SLOTS];
    ++g_io_writes;
    if (!blockdev::write_sector(sector, in)) {
        if (c.valid && c.sector == sector) c.valid = false;
        return false;
    }
    c.sector = sector;
    c.valid = true;
    __builtin_memcpy(c.data, in, SECTOR_SIZE);
    return true;
}

static void cache_drop(u64 first, u64 count) {
    if (count >= CACHE_SLOTS) {
        for (auto& c : g_cache)
            if (c.valid && c.sector >= first && c.sector < first + count) c.valid = false;
        return;
    }
    for (u64 i = 0; i < count; ++i) {
        CacheSlot& c = g_cache[(first + i) % CACHE_SLOTS];
        if (c.valid && c.sector == first + i) c.valid = false;
    }
}

static void bm_set(u64 s, bool used) {
    if (used) g_bitmap[s / 8] |= static_cast<u8>(1u << (s % 8));
    else g_bitmap[s / 8] &= static_cast<u8>(~(1u << (s % 8)));
    u64 idx = (s / 8) / SECTOR_SIZE;
    g_bm_dirty[idx] = 1;
    if (idx < g_bm_lo) g_bm_lo = idx;
    if (idx > g_bm_hi) g_bm_hi = idx;
}

static void commit() {
    if (!g_mounted) return;
    if (g_bm_lo <= g_bm_hi) {
        for (u64 i = g_bm_lo; i <= g_bm_hi; ++i) {
            if (!g_bm_dirty[i]) continue;
            raw_wr(g_sb.bitmap_start + i, g_bitmap + i * SECTOR_SIZE);
            g_bm_dirty[i] = 0;
        }
        g_bm_lo = ~0ull;
        g_bm_hi = 0;
    }
    if (g_sb_dirty) {
        u8 buf[SECTOR_SIZE];
        for (auto& b : buf) b = 0;
        __builtin_memcpy(buf, &g_sb, sizeof(g_sb));
        raw_wr(0, buf);
        g_sb_dirty = false;
    }
}

static u32 g_txn_depth = 0;

struct Txn {
    Txn() { ++g_txn_depth; }
    ~Txn() { if (--g_txn_depth == 0) commit(); }
};

void begin_batch() { ++g_txn_depth; }

bool end_batch() {
    if (g_txn_depth == 0) return false;
    if (--g_txn_depth == 0) commit();
    return true;
}

static u64 find_run(u64 from, u64 to, u64 count) {
    u64 run = 0, run_start = 0;
    for (u64 s = from; s < to;) {
        if ((s & 7) == 0 && g_bitmap[s / 8] == 0xFF) { run = 0; s += 8; continue; }
        if ((s & 7) == 0 && s + 8 <= to && g_bitmap[s / 8] == 0x00) {
            if (run == 0) run_start = s;
            run += 8;
            if (run >= count) return run_start;
            s += 8;
            continue;
        }
        if (!(g_bitmap[s / 8] & (1u << (s % 8)))) {
            if (run == 0) run_start = s;
            if (++run == count) return run_start;
        } else {
            run = 0;
        }
        ++s;
    }
    return 0;
}

static u64 alloc_sectors(u64 count) {
    u64 data_start = g_sb.bitmap_start + g_sb.bitmap_sectors;
    u64 hint = g_alloc_hint < data_start ? data_start : g_alloc_hint;
    u64 r = find_run(hint, g_sb.total_sectors, count);
    if (!r) {
        u64 end = hint + count;
        if (end > g_sb.total_sectors) end = g_sb.total_sectors;
        r = find_run(data_start, end, count);
    }
    if (!r) return 0;
    for (u64 j = r; j < r + count; ++j) bm_set(j, true);
    g_alloc_hint = r + count;
    return r;
}

static void release_sectors(u64 first, u64 count) {
    for (u64 j = first; j < first + count; ++j) bm_set(j, false);
    if (first < g_alloc_hint) g_alloc_hint = first;
}

u64 free_space_sectors() {
    if (!g_mounted) return 0;
    u64 data_start = g_sb.bitmap_start + g_sb.bitmap_sectors;
    u64 n = 0;
    for (u64 s = data_start; s < g_sb.total_sectors; ++s)
        if (!(g_bitmap[s / 8] & (1u << (s % 8)))) ++n;
    return n;
}

static u64 alloc_star_id() {
    u64 id = g_sb.next_star_id++;
    g_sb_dirty = true;
    return id;
}

static bool view_root(u64 snap, u64* root) {
    if (snap == LIVE) { *root = g_sb.catalog_root; return true; }
    if (snap > g_sb.snap_count) return false;
    *root = g_sb.snaps[snap - 1].catalog_root;
    return *root != 0;
}

static bool bt_search(u64 root, u64 key, StarEntry* out) {
    u8 buf[SECTOR_SIZE];
    u64 sector = root;
    for (int depth = 0; depth < 32; ++depth) {
        if (!rd(sector, buf)) return false;
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
        sector = node->child[i];
    }
    return false;
}

static u64 place_node(u64 sector, u8* buf) {
    auto* h = reinterpret_cast<NodeHeader*>(buf);
    if (sector != 0 && h->gen == g_sb.epoch) {
        if (!wr(sector, buf)) return 0;
        return sector;
    }
    u64 ns = alloc_sectors(1);
    if (!ns) return 0;
    h->gen = g_sb.epoch;
    if (!wr(ns, buf)) return 0;
    return ns;
}

struct Up {
    u64 sector;
    bool split;
    u64 promoted;
    u64 right;
};

static bool bt_upsert(u64 node_sector, u64 key, const StarEntry& value, Up* out) {
    u8 buf[SECTOR_SIZE];
    if (!rd(node_sector, buf)) return false;

    if (buf[0]) {
        auto* leaf = reinterpret_cast<LeafNode*>(buf);
        LeafEntry tmp[LEAF_MAX + 1];
        u32 pos = 0;
        while (pos < leaf->count && leaf->entries[pos].star_id < key) ++pos;
        u32 total;
        if (pos < leaf->count && leaf->entries[pos].star_id == key) {
            for (u32 i = 0; i < leaf->count; ++i) tmp[i] = leaf->entries[i];
            tmp[pos].entry = value;
            total = leaf->count;
        } else {
            for (u32 i = 0; i < pos; ++i) tmp[i] = leaf->entries[i];
            tmp[pos].star_id = key;
            tmp[pos].entry = value;
            for (u32 i = pos; i < leaf->count; ++i) tmp[i + 1] = leaf->entries[i];
            total = leaf->count + 1;
        }

        if (total <= LEAF_MAX) {
            leaf->count = total;
            for (u32 i = 0; i < total; ++i) leaf->entries[i] = tmp[i];
            u64 s = place_node(node_sector, buf);
            if (!s) return false;
            *out = { s, false, 0, 0 };
            return true;
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

        u64 rs = place_node(0, rbuf);
        if (!rs) return false;
        u64 ls = place_node(node_sector, buf);
        if (!ls) return false;
        *out = { ls, true, tmp[left_count].star_id, rs };
        return true;
    }

    auto* node = reinterpret_cast<InternalNode*>(buf);
    u32 i = 0;
    while (i < node->count && key >= node->key[i]) ++i;

    Up c;
    if (!bt_upsert(node->child[i], key, value, &c)) return false;
    node->child[i] = c.sector;

    if (!c.split) {
        u64 s = place_node(node_sector, buf);
        if (!s) return false;
        *out = { s, false, 0, 0 };
        return true;
    }

    u64 tmp_key[INTERNAL_MAX + 1];
    u64 tmp_child[INTERNAL_MAX + 2];
    for (u32 j = 0; j < i; ++j) tmp_key[j] = node->key[j];
    for (u32 j = 0; j <= i; ++j) tmp_child[j] = node->child[j];
    tmp_key[i] = c.promoted;
    tmp_child[i + 1] = c.right;
    for (u32 j = i; j < node->count; ++j) tmp_key[j + 1] = node->key[j];
    for (u32 j = i + 1; j <= node->count; ++j) tmp_child[j + 1] = node->child[j];
    u32 total = node->count + 1;

    if (total <= INTERNAL_MAX) {
        node->count = total;
        for (u32 j = 0; j < total; ++j) node->key[j] = tmp_key[j];
        for (u32 j = 0; j <= total; ++j) node->child[j] = tmp_child[j];
        u64 s = place_node(node_sector, buf);
        if (!s) return false;
        *out = { s, false, 0, 0 };
        return true;
    }

    u32 mid = total / 2;
    u64 up_key = tmp_key[mid];
    node->count = mid;
    for (u32 j = 0; j < mid; ++j) node->key[j] = tmp_key[j];
    for (u32 j = 0; j <= mid; ++j) node->child[j] = tmp_child[j];

    u8 rbuf[SECTOR_SIZE];
    for (auto& b : rbuf) b = 0;
    auto* rnode = reinterpret_cast<InternalNode*>(rbuf);
    rnode->is_leaf = 0;
    u32 right_count = total - mid - 1;
    rnode->count = right_count;
    for (u32 j = 0; j < right_count; ++j) rnode->key[j] = tmp_key[mid + 1 + j];
    for (u32 j = 0; j <= right_count; ++j) rnode->child[j] = tmp_child[mid + 1 + j];

    u64 rs = place_node(0, rbuf);
    if (!rs) return false;
    u64 ls = place_node(node_sector, buf);
    if (!ls) return false;
    *out = { ls, true, up_key, rs };
    return true;
}

static bool catalog_upsert(u64 key, const StarEntry& value) {
    Up r;
    if (!bt_upsert(g_sb.catalog_root, key, value, &r)) return false;
    u64 root = r.sector;
    if (r.split) {
        u8 buf[SECTOR_SIZE];
        for (auto& b : buf) b = 0;
        auto* n = reinterpret_cast<InternalNode*>(buf);
        n->is_leaf = 0;
        n->count = 1;
        n->key[0] = r.promoted;
        n->child[0] = r.sector;
        n->child[1] = r.right;
        root = place_node(0, buf);
        if (!root) return false;
    }
    if (root != g_sb.catalog_root) {
        g_sb.catalog_root = root;
        g_sb_dirty = true;
    }
    return true;
}

static bool catalog_find(u64 key, StarEntry* out) { return bt_search(g_sb.catalog_root, key, out); }

static bool write_extent(u64 start, u64 nsec, const void* data, u64 size) {
    const u8* src = static_cast<const u8*>(data);
    u64 full = size / SECTOR_SIZE;
    if (full > 0 && blockdev::write_sectors(start, full, src)) {
        g_io_writes += full;
        cache_drop(start, full);
    } else {
        full = 0;
    }
    u8 buf[SECTOR_SIZE];
    for (u64 i = full; i < nsec; ++i) {
        u64 done = i * SECTOR_SIZE;
        u64 remaining = done < size ? size - done : 0;
        u64 chunk = remaining < SECTOR_SIZE ? remaining : SECTOR_SIZE;
        for (u64 b = 0; b < SECTOR_SIZE; ++b) buf[b] = (b < chunk) ? src[done + b] : 0;
        if (!wr(start + i, buf)) return false;
    }
    return true;
}

static bool dir_make_current(StarEntry& e, u64 dir_star) {
    {
        u8 head[SECTOR_SIZE];
        if (e.first_sector != 0) {
            if (!rd(e.first_sector, head)) return false;
            if (reinterpret_cast<DirSector*>(head)->gen == g_sb.epoch) return true;
        }
    }
    u8 prev[SECTOR_SIZE];
    u64 prev_sector = 0;
    bool prev_dirty = false;
    bool head_changed = false;
    u64 sector = e.first_sector;

    while (sector != 0) {
        u8 cur[SECTOR_SIZE];
        if (!rd(sector, cur)) return false;
        auto* ds = reinterpret_cast<DirSector*>(cur);
        u64 orig_next = ds->next_sector;
        u64 final_sector = sector;
        bool dirty = false;
        if (ds->gen != g_sb.epoch) {
            final_sector = alloc_sectors(1);
            if (!final_sector) return false;
            ds->gen = g_sb.epoch;
            dirty = true;
        }
        if (prev_sector == 0) {
            if (final_sector != e.first_sector) { e.first_sector = final_sector; head_changed = true; }
        } else {
            auto* pd = reinterpret_cast<DirSector*>(prev);
            if (pd->next_sector != final_sector) { pd->next_sector = final_sector; prev_dirty = true; }
            if (prev_dirty && !wr(prev_sector, prev)) return false;
        }
        __builtin_memcpy(prev, cur, SECTOR_SIZE);
        prev_sector = final_sector;
        prev_dirty = dirty;
        sector = orig_next;
    }
    if (prev_sector && prev_dirty && !wr(prev_sector, prev)) return false;
    if (head_changed && !catalog_upsert(dir_star, e)) return false;
    return true;
}

static u64 g_hint_dir = INVALID_STAR;
static u64 g_hint_sector = 0;
static u64 g_hint_epoch = 0;

static void reset_runtime_state() {
    g_hint_dir = INVALID_STAR;
    g_hint_sector = 0;
    g_hint_epoch = 0;
    g_txn_depth = 0;
}

static bool add_edge(u64 dir_star, const char* name, u64 target) {
    StarEntry e;
    if (!catalog_find(dir_star, &e) || e.type != TYPE_CONSTELLATION) return false;
    if (!dir_make_current(e, dir_star)) return false;

    u64 sector = e.first_sector;
    if (g_hint_dir == dir_star && g_hint_epoch == g_sb.epoch && g_hint_sector != 0)
        sector = g_hint_sector;
    u8 buf[SECTOR_SIZE];
    for (;;) {
        if (!rd(sector, buf)) return false;
        auto* ds = reinterpret_cast<DirSector*>(buf);
        for (u32 i = 0; i < DIR_ENTRIES_PER_SECTOR; ++i) {
            if (!ds->entries[i].in_use) {
                ds->entries[i].in_use = 1;
                ds->entries[i].star = target;
                copy_name(ds->entries[i].name, name);
                if (!wr(sector, buf)) return false;
                g_hint_dir = dir_star;
                g_hint_sector = sector;
                g_hint_epoch = g_sb.epoch;
                return true;
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
        reinterpret_cast<DirSector*>(zero)->gen = g_sb.epoch;
        if (!wr(new_sector, zero)) return false;

        ds->next_sector = new_sector;
        if (!wr(sector, buf)) return false;

        e.sector_count += 1;
        if (!catalog_upsert(dir_star, e)) return false;
        sector = new_sector;
    }
}

bool format(u64 total_sectors) {
    for (auto& c : g_cache) c.valid = false;
    g_sb = Superblock{};
    g_sb.magic = MAGIC;
    g_sb.version = VERSION;
    g_sb.sector_size = SECTOR_SIZE;
    g_sb.total_sectors = total_sectors;
    g_sb.bitmap_start = 1;
    g_sb.bitmap_sectors = (total_sectors / 8 + SECTOR_SIZE - 1) / SECTOR_SIZE;
    if (g_sb.bitmap_sectors == 0) g_sb.bitmap_sectors = 1;
    g_sb.next_star_id = 0;
    g_sb.epoch = 1;
    g_sb.snap_count = 0;

    g_bitmap = static_cast<u8*>(alloc_ram(g_sb.bitmap_sectors * SECTOR_SIZE));
    g_bm_dirty = static_cast<u8*>(alloc_ram(g_sb.bitmap_sectors));
    for (u64 i = 0; i < g_sb.bitmap_sectors; ++i) g_bm_dirty[i] = 1;
    g_bm_lo = 0;
    g_bm_hi = g_sb.bitmap_sectors - 1;
    g_alloc_hint = g_sb.bitmap_start + g_sb.bitmap_sectors;
    g_sb_dirty = true;
    g_mounted = true;
    reset_runtime_state();

    u64 root_sector = alloc_sectors(1);
    if (root_sector == 0) return false;
    u8 buf[SECTOR_SIZE];
    for (auto& b : buf) b = 0;
    auto* leaf = reinterpret_cast<LeafNode*>(buf);
    leaf->is_leaf = 1;
    leaf->gen = g_sb.epoch;
    if (!wr(root_sector, buf)) return false;
    g_sb.catalog_root = root_sector;
    commit();

    u64 root = create_constellation(INVALID_STAR, "");
    if (root != ROOT_STAR) {
        serial::writeln("[stellar] format: root did not land at star 0");
        return false;
    }
    serial::printf("[stellar] formatted: %lu sectors, COW B+tree catalog (leaf fanout %u, internal fanout %u), directory fanout %u/sector, O(1) epoch snapshots (max %u)\n",
                    total_sectors, LEAF_MAX, INTERNAL_MAX, DIR_ENTRIES_PER_SECTOR, SNAPSHOT_MAX);
    return true;
}

bool mount() {
    u8 buf[SECTOR_SIZE];
    for (auto& c : g_cache) c.valid = false;
    if (!blockdev::read_sector(0, buf)) return false;
    ++g_io_reads;
    Superblock sb;
    __builtin_memcpy(&sb, buf, sizeof(sb));
    if (sb.magic != MAGIC || sb.version != VERSION) {
        serial::writeln("[stellar] mount: bad magic or version, not formatted");
        return false;
    }
    g_sb = sb;
    g_bitmap = static_cast<u8*>(alloc_ram(g_sb.bitmap_sectors * SECTOR_SIZE));
    g_bm_dirty = static_cast<u8*>(alloc_ram(g_sb.bitmap_sectors));
    g_bm_lo = ~0ull;
    g_bm_hi = 0;
    for (u64 i = 0; i < g_sb.bitmap_sectors; ++i) {
        blockdev::read_sector(g_sb.bitmap_start + i, g_bitmap + i * SECTOR_SIZE);
        ++g_io_reads;
    }
    g_alloc_hint = g_sb.bitmap_start + g_sb.bitmap_sectors;
    g_sb_dirty = false;
    g_mounted = true;
    reset_runtime_state();
    serial::printf("[stellar] mounted: %lu sectors, catalog root at sector %lu, epoch %lu, %u snapshot(s), next star id %lu\n",
                    g_sb.total_sectors, g_sb.catalog_root, g_sb.epoch, g_sb.snap_count, g_sb.next_star_id);
    return true;
}

u64 create_constellation(u64 parent, const char* name) {
    if (!g_mounted) return INVALID_STAR;
    Txn txn;
    u64 id = alloc_star_id();
    u64 start = alloc_sectors(1);
    if (start == 0) return INVALID_STAR;

    u8 zero[SECTOR_SIZE];
    for (auto& b : zero) b = 0;
    reinterpret_cast<DirSector*>(zero)->gen = g_sb.epoch;
    if (!wr(start, zero)) return INVALID_STAR;

    StarEntry entry{};
    entry.type = TYPE_CONSTELLATION;
    entry.first_sector = start;
    entry.sector_count = 1;
    entry.gen = g_sb.epoch;
    entry.nlink = 1;
    if (!catalog_upsert(id, entry)) return INVALID_STAR;

    if (parent != INVALID_STAR && !add_edge(parent, name, id)) return INVALID_STAR;
    return id;
}

u64 create_file(u64 parent, const char* name, const void* data, u64 size) {
    if (!g_mounted) return INVALID_STAR;
    Txn txn;
    u64 id = alloc_star_id();
    u64 nsec = size == 0 ? 1 : (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    u64 start = alloc_sectors(nsec);
    if (start == 0) return INVALID_STAR;
    if (!write_extent(start, nsec, data, size)) return INVALID_STAR;

    StarEntry entry{};
    entry.type = TYPE_FILE;
    entry.size_bytes = size;
    entry.first_sector = start;
    entry.sector_count = static_cast<u32>(nsec);
    entry.gen = g_sb.epoch;
    entry.nlink = 1;
    entry.checksum = crc32_full(static_cast<const u8*>(data), size);
    if (!catalog_upsert(id, entry)) return INVALID_STAR;

    if (parent != INVALID_STAR && !add_edge(parent, name, id)) return INVALID_STAR;
    return id;
}

u64 write_file(u64 star, const void* data, u64 size) {
    if (!g_mounted) return INVALID_STAR;
    Txn txn;
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
    new_e.sector_count = static_cast<u32>(nsec);
    new_e.gen = g_sb.epoch;
    new_e.nlink = old_e.nlink;
    new_e.checksum = crc32_full(static_cast<const u8*>(data), size);

    if (!catalog_upsert(star, new_e)) return INVALID_STAR;
    if (old_e.gen == g_sb.epoch) release_sectors(old_e.first_sector, old_e.sector_count);
    return star;
}

u64 snapshot() {
    if (!g_mounted) return INVALID_STAR;
    u32 slot = g_sb.snap_count;
    for (u32 i = 0; i < g_sb.snap_count; ++i) {
        if (g_sb.snaps[i].catalog_root == 0) { slot = i; break; }
    }
    if (slot >= SNAPSHOT_MAX) return INVALID_STAR;
    Txn txn;
    g_sb.snaps[slot].catalog_root = g_sb.catalog_root;
    g_sb.snaps[slot].epoch = g_sb.epoch;
    if (slot == g_sb.snap_count) ++g_sb.snap_count;
    ++g_sb.epoch;
    g_sb_dirty = true;
    return static_cast<u64>(slot) + 1;
}

bool link(u64 dir_star, const char* name, u64 target) {
    if (!g_mounted || dir_star == target) return false;
    Txn txn;
    StarEntry e;
    if (!catalog_find(target, &e) || e.type == TYPE_FREE) return false;
    if (!add_edge(dir_star, name, target)) return false;
    if (!catalog_find(target, &e)) return false;
    ++e.nlink;
    return catalog_upsert(target, e);
}

u32 live_snapshot_count() {
    u32 n = 0;
    for (u32 i = 0; i < g_sb.snap_count; ++i)
        if (g_sb.snaps[i].catalog_root != 0) ++n;
    return n;
}

bool delete_snapshot(u64 snap) {
    if (!g_mounted || snap == LIVE || snap > g_sb.snap_count) return false;
    if (g_sb.snaps[snap - 1].catalog_root == 0) return false;
    Txn txn;
    g_sb.snaps[snap - 1].catalog_root = 0;
    g_sb.snaps[snap - 1].epoch = 0;
    while (g_sb.snap_count > 0 && g_sb.snaps[g_sb.snap_count - 1].catalog_root == 0)
        --g_sb.snap_count;
    g_sb_dirty = true;
    return true;
}

static bool dir_is_empty(const StarEntry& d) {
    u64 sector = d.first_sector;
    u8 buf[SECTOR_SIZE];
    while (sector != 0) {
        if (!rd(sector, buf)) return false;
        auto* ds = reinterpret_cast<DirSector*>(buf);
        for (u32 i = 0; i < DIR_ENTRIES_PER_SECTOR; ++i)
            if (ds->entries[i].in_use) return false;
        sector = ds->next_sector;
    }
    return true;
}

static bool remove_edge(u64 dir_star, const char* name, u64* target_out) {
    StarEntry e;
    if (!catalog_find(dir_star, &e) || e.type != TYPE_CONSTELLATION) return false;
    if (!dir_make_current(e, dir_star)) return false;

    u64 sector = e.first_sector;
    u8 buf[SECTOR_SIZE];
    while (sector != 0) {
        if (!rd(sector, buf)) return false;
        auto* ds = reinterpret_cast<DirSector*>(buf);
        for (u32 i = 0; i < DIR_ENTRIES_PER_SECTOR; ++i) {
            if (ds->entries[i].in_use && names_equal(ds->entries[i].name, name)) {
                g_hint_dir = INVALID_STAR;
                *target_out = ds->entries[i].star;
                ds->entries[i].in_use = 0;
                ds->entries[i].star = 0;
                ds->entries[i].name[0] = 0;
                return wr(sector, buf);
            }
        }
        sector = ds->next_sector;
    }
    return false;
}

static void release_current_epoch(const StarEntry& t) {
    if (t.type == TYPE_FILE) {
        if (t.gen == g_sb.epoch) release_sectors(t.first_sector, t.sector_count);
        return;
    }
    u64 sector = t.first_sector;
    u8 buf[SECTOR_SIZE];
    while (sector != 0) {
        if (!rd(sector, buf)) return;
        auto* ds = reinterpret_cast<DirSector*>(buf);
        u64 next = ds->next_sector;
        if (ds->gen == g_sb.epoch) release_sectors(sector, 1);
        sector = next;
    }
}

bool unlink(u64 dir_star, const char* name) {
    if (!g_mounted) return false;
    Txn txn;
    u64 target = find(dir_star, name);
    if (target == INVALID_STAR || target == ROOT_STAR) return false;

    StarEntry t;
    if (!catalog_find(target, &t)) return false;
    if (t.type == TYPE_CONSTELLATION && !dir_is_empty(t)) return false;

    u64 removed = INVALID_STAR;
    if (!remove_edge(dir_star, name, &removed) || removed != target) return false;

    if (!catalog_find(target, &t)) return false;
    if (t.nlink > 1) {
        --t.nlink;
        return catalog_upsert(target, t);
    }
    release_current_epoch(t);
    t.type = TYPE_FREE;
    t.nlink = 0;
    t.size_bytes = 0;
    t.first_sector = 0;
    t.sector_count = 0;
    t.checksum = 0;
    return catalog_upsert(target, t);
}

static u8* g_mark = nullptr;

static bool mk_test(u64 s) { return ((g_mark[s / 8] >> (s % 8)) & 1u) != 0; }
static void mk_set(u64 s) { g_mark[s / 8] |= static_cast<u8>(1u << (s % 8)); }
static void mk_range(u64 first, u64 count) {
    for (u64 s = first; s < first + count && s < g_sb.total_sectors; ++s) mk_set(s);
}

static void free_ram(void* p, u64 bytes) {
    int order = 0;
    while ((universe::PAGE_SIZE << order) < bytes) ++order;
    universe::free(reinterpret_cast<u64>(p) - g_hhdm, order);
}

static bool mark_dir_chain(u64 sector) {
    u8 buf[SECTOR_SIZE];
    while (sector != 0 && sector < g_sb.total_sectors && !mk_test(sector)) {
        mk_set(sector);
        if (!rd(sector, buf)) return false;
        sector = reinterpret_cast<DirSector*>(buf)->next_sector;
    }
    return true;
}

static bool mark_tree(u64 sector, u32 depth) {
    if (depth > 12 || sector == 0 || sector >= g_sb.total_sectors) return false;
    if (mk_test(sector)) return true;
    mk_set(sector);

    u8 buf[SECTOR_SIZE];
    if (!rd(sector, buf)) return false;

    if (buf[0]) {
        auto* leaf = reinterpret_cast<LeafNode*>(buf);
        u32 n = leaf->count;
        if (n > LEAF_MAX) n = LEAF_MAX;
        for (u32 i = 0; i < n; ++i) {
            StarEntry e = leaf->entries[i].entry;
            if (e.type == TYPE_FILE) mk_range(e.first_sector, e.sector_count);
            else if (e.type == TYPE_CONSTELLATION && !mark_dir_chain(e.first_sector)) return false;
        }
        return true;
    }

    auto* node = reinterpret_cast<InternalNode*>(buf);
    u32 n = node->count;
    if (n > INTERNAL_MAX) n = INTERNAL_MAX;
    for (u32 i = 0; i <= n; ++i)
        if (!mark_tree(node->child[i], depth + 1)) return false;
    return true;
}

u64 gc() {
    if (!g_mounted) return INVALID_STAR;
    Txn txn;
    u64 bm_bytes = g_sb.bitmap_sectors * SECTOR_SIZE;
    u64 data_start = g_sb.bitmap_start + g_sb.bitmap_sectors;
    g_mark = static_cast<u8*>(alloc_ram(bm_bytes));
    mk_range(0, data_start);

    bool ok = mark_tree(g_sb.catalog_root, 0);
    for (u32 i = 0; ok && i < g_sb.snap_count; ++i)
        if (g_sb.snaps[i].catalog_root != 0) ok = mark_tree(g_sb.snaps[i].catalog_root, 0);

    u64 freed = INVALID_STAR;
    if (ok) {
        freed = 0;
        for (u64 s = data_start; s < g_sb.total_sectors; ++s) {
            bool used = ((g_bitmap[s / 8] >> (s % 8)) & 1u) != 0;
            if (used && !mk_test(s)) ++freed;
        }
        for (u64 i = 0; i < g_sb.bitmap_sectors; ++i) {
            u8* cur = g_bitmap + i * SECTOR_SIZE;
            const u8* want = g_mark + i * SECTOR_SIZE;
            if (__builtin_memcmp(cur, want, SECTOR_SIZE) == 0) continue;
            __builtin_memcpy(cur, want, SECTOR_SIZE);
            g_bm_dirty[i] = 1;
            if (i < g_bm_lo) g_bm_lo = i;
            if (i > g_bm_hi) g_bm_hi = i;
        }
        g_alloc_hint = data_start;
    }

    free_ram(g_mark, bm_bytes);
    g_mark = nullptr;
    return freed;
}

bool verify_file(u64 star, u64 snap) {
    if (!g_mounted) return false;
    u64 root;
    if (!view_root(snap, &root)) return false;
    StarEntry e;
    if (!bt_search(root, star, &e) || e.type != TYPE_FILE) return false;

    constexpr u64 CHUNK = 16;
    static u8* scratch = nullptr;
    if (!scratch) scratch = static_cast<u8*>(alloc_ram(CHUNK * SECTOR_SIZE));

    u32 crc = 0xFFFFFFFFu;
    u64 remaining = e.size_bytes;
    u64 sec = 0;
    while (remaining > 0 && sec < e.sector_count) {
        u64 n = e.sector_count - sec;
        if (n > CHUNK) n = CHUNK;
        g_io_reads += n;
        if (!blockdev::read_sectors(e.first_sector + sec, n, scratch)) return false;
        u64 bytes = n * SECTOR_SIZE;
        if (bytes > remaining) bytes = remaining;
        crc = crc32_update(crc, scratch, bytes);
        remaining -= bytes;
        sec += n;
    }
    crc ^= 0xFFFFFFFFu;

    if (crc != e.checksum) {
        serial::printf("[stellar] checksum mismatch on star %lu: stored %x computed %x\n",
                        star, e.checksum, crc);
        return false;
    }
    return true;
}

u64 read_file(u64 star, void* buf, u64 max_size, u64 snap) {
    if (!g_mounted) return 0;
    u64 root;
    if (!view_root(snap, &root)) return 0;
    StarEntry e;
    if (!bt_search(root, star, &e) || e.type != TYPE_FILE) return 0;
    u64 size = e.size_bytes;
    u64 to_read = size < max_size ? size : max_size;
    u8* dst = static_cast<u8*>(buf);
    u64 read_so_far = 0;
    u64 full = to_read / SECTOR_SIZE;
    if (full >= 2 && blockdev::read_sectors(e.first_sector, full, dst)) {
        g_io_reads += full;
        read_so_far = full * SECTOR_SIZE;
    }
    u8 sector_buf[SECTOR_SIZE];
    for (u64 i = read_so_far / SECTOR_SIZE; i < e.sector_count && read_so_far < to_read; ++i) {
        if (!rd(e.first_sector + i, sector_buf)) break;
        u64 chunk = to_read - read_so_far;
        if (chunk > SECTOR_SIZE) chunk = SECTOR_SIZE;
        for (u64 b = 0; b < chunk; ++b) dst[read_so_far + b] = sector_buf[b];
        read_so_far += chunk;
    }
    return read_so_far;
}

u64 find(u64 dir_star, const char* name, u64 snap) {
    if (!g_mounted) return INVALID_STAR;
    u64 root;
    if (!view_root(snap, &root)) return INVALID_STAR;
    StarEntry e;
    if (!bt_search(root, dir_star, &e) || e.type != TYPE_CONSTELLATION) return INVALID_STAR;
    u64 sector = e.first_sector;
    u8 buf[SECTOR_SIZE];
    while (sector != 0) {
        if (!rd(sector, buf)) return INVALID_STAR;
        auto* ds = reinterpret_cast<DirSector*>(buf);
        for (u32 i = 0; i < DIR_ENTRIES_PER_SECTOR; ++i)
            if (ds->entries[i].in_use && names_equal(ds->entries[i].name, name)) return ds->entries[i].star;
        sector = ds->next_sector;
    }
    return INVALID_STAR;
}

void list(u64 dir_star, ListCallback cb, void* ctx, u64 snap) {
    if (!g_mounted) return;
    u64 root;
    if (!view_root(snap, &root)) return;
    StarEntry e;
    if (!bt_search(root, dir_star, &e) || e.type != TYPE_CONSTELLATION) return;
    u64 sector = e.first_sector;
    u8 buf[SECTOR_SIZE];
    while (sector != 0) {
        if (!rd(sector, buf)) return;
        auto* ds = reinterpret_cast<DirSector*>(buf);
        for (u32 i = 0; i < DIR_ENTRIES_PER_SECTOR; ++i) {
            if (!ds->entries[i].in_use) continue;
            StarEntry child;
            u32 type = bt_search(root, ds->entries[i].star, &child) ? child.type : TYPE_FREE;
            cb(ds->entries[i].name, ds->entries[i].star, type, ctx);
        }
        sector = ds->next_sector;
    }
}

}
