#include <cosmos/stellar.hpp>
#include <cosmos/blockdev.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/orbital.hpp>

namespace stellar {

constexpr u64 SECTOR_SIZE = 512;
constexpr u64 MAGIC = 0x5AE1157A6111A2C5ull;
constexpr u32 VERSION = 6;
constexpr u32 NAME_LEN = 52;
constexpr u32 LEAF_MAX = 7;
constexpr u32 INTERNAL_MAX = 30;
constexpr u32 CACHE_SLOTS = 256;
constexpr u64 MIN_SECTORS = 64;
constexpr u32 CRC_OFF = SECTOR_SIZE - 4;
constexpr u64 BM_PAYLOAD = CRC_OFF;
constexpr u32 SB_SLOTS = 2;
constexpr u32 FEATURES_INCOMPAT_KNOWN = 0;

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
    u64 seq;
    u32 features_incompat;
    u32 features_compat;
    u32 snap_count;
    u32 reserved;
    SnapRec snaps[SNAPSHOT_MAX];
    u8 padding[CRC_OFF - 88 - SNAPSHOT_MAX * sizeof(SnapRec)];
    u32 crc;
};
static_assert(sizeof(Superblock) == SECTOR_SIZE, "superblock must fill one sector");

struct PACKED StarEntry {
    u32 type;
    u32 checksum;
    u64 size_bytes;
    u64 first_sector;
    u64 gen;
    u32 sector_count;
    u32 nlink;
    u64 mtime;
    u32 flags;
    u32 reserved1;
};
static_assert(sizeof(StarEntry) == 56, "StarEntry must be 56 bytes");

struct PACKED DirEntry {
    u32 in_use;
    u64 star;
    char name[NAME_LEN];
};
static_assert(sizeof(DirEntry) == 64, "DirEntry must be 64 bytes");

constexpr u32 DIR_ENTRIES_PER_SECTOR = (CRC_OFF - 16) / sizeof(DirEntry);

struct PACKED DirSector {
    u64 next_sector;
    u64 gen;
    DirEntry entries[DIR_ENTRIES_PER_SECTOR];
    u8 padding[CRC_OFF - 16 - DIR_ENTRIES_PER_SECTOR * sizeof(DirEntry)];
    u32 crc;
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
static_assert(sizeof(LeafEntry) == 64, "LeafEntry must be 64 bytes");

struct PACKED LeafNode {
    u8 is_leaf;
    u8 reserved0[3];
    u32 count;
    u64 gen;
    LeafEntry entries[LEAF_MAX];
    u8 padding[CRC_OFF - 16 - LEAF_MAX * sizeof(LeafEntry)];
    u32 crc;
};
static_assert(sizeof(LeafNode) == SECTOR_SIZE, "LeafNode must fill one sector");

struct PACKED InternalNode {
    u8 is_leaf;
    u8 reserved0[3];
    u32 count;
    u64 gen;
    u64 key[INTERNAL_MAX];
    u64 child[INTERNAL_MAX + 1];
    u8 padding[CRC_OFF - 16 - INTERNAL_MAX * 8 - (INTERNAL_MAX + 1) * 8];
    u32 crc;
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

static orbital::Mutex g_lock;

struct Guard {
    Guard() { g_lock.lock(); }
    ~Guard() { g_lock.unlock(); }
};

static Status g_err = Status::Ok;
static u32 g_api_depth = 0;

static bool fail(Status s) {
    if (g_err == Status::Ok) g_err = s;
    return false;
}
static u64 fail_id(Status s) {
    fail(s);
    return INVALID_STAR;
}

struct Api {
    Status* why;
    explicit Api(Status* w) : why(w) {
        g_lock.lock();
        if (g_api_depth++ == 0) g_err = Status::Ok;
    }
    ~Api() {
        --g_api_depth;
        g_lock.unlock();
    }
    void report(bool good) {
        if (why) *why = good ? Status::Ok : (g_err != Status::Ok ? g_err : Status::Internal);
    }
    bool ok(bool v) { report(v); return v; }
    u64 val(u64 v) { report(v != INVALID_STAR); return v; }
};

const char* status_name(Status s) {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::NotMounted: return "not mounted";
        case Status::NotFormatted: return "not formatted";
        case Status::InvalidArgument: return "invalid argument";
        case Status::InvalidName: return "invalid name";
        case Status::NotFound: return "not found";
        case Status::Exists: return "already exists";
        case Status::NotADirectory: return "not a directory";
        case Status::IsADirectory: return "is a directory";
        case Status::NotEmpty: return "directory not empty";
        case Status::NoSpace: return "no space left on device";
        case Status::NoMemory: return "out of memory";
        case Status::Io: return "I/O error";
        case Status::Checksum: return "checksum mismatch";
        case Status::Corrupt: return "corrupt metadata";
        case Status::Busy: return "busy (batch open)";
        case Status::NoSuchSnapshot: return "no such snapshot";
        case Status::TooManySnapshots: return "snapshot table full";
        case Status::Unsupported: return "unsupported on-disk features";
        case Status::Internal: return "internal error";
    }
    return "unknown";
}

static u64 (*g_clock_fn)() = nullptr;
static u64 now() { return g_clock_fn ? g_clock_fn() : 0; }
void set_clock(u64 (*fn)()) { g_clock_fn = fn; }

static void idx_drop_all();
static void bloom_drop_all();

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

static bool valid_name(const char* n) {
    if (!n) return false;
    u32 len = 0;
    while (len < NAME_LEN && n[len]) {
        if (n[len] == '/') return false;
        ++len;
    }
    if (len == 0 || len >= NAME_LEN) return false;
    if (len == 1 && n[0] == '.') return false;
    if (len == 2 && n[0] == '.' && n[1] == '.') return false;
    return true;
}

void init(u64 hhdm_offset) { g_hhdm = hhdm_offset; }

IoStats io_stats() { Guard guard; return { g_io_reads, g_io_writes, g_cache_hits }; }
u32 snapshot_count() { Guard guard; return g_sb.snap_count; }

static void* alloc_ram(u64 bytes) {
    int order = 0;
    while ((universe::PAGE_SIZE << order) < bytes) ++order;
    if (order > universe::MAX_ORDER) { fail(Status::NoMemory); return nullptr; }
    u64 phys = universe::alloc(order);
    if (phys == 0) { fail(Status::NoMemory); return nullptr; }
    void* v = reinterpret_cast<void*>(g_hhdm + phys);
    __builtin_memset(v, 0, universe::PAGE_SIZE << order);
    return v;
}

static void* alloc_ram_soft(u64 bytes) {
    Status saved = g_err;
    void* p = alloc_ram(bytes);
    if (!p) g_err = saved;
    return p;
}

static void free_ram(void* p, u64 bytes) {
    if (!p) return;
    int order = 0;
    while ((universe::PAGE_SIZE << order) < bytes) ++order;
    universe::free(reinterpret_cast<u64>(p) - g_hhdm, order);
}

constexpr u64 VERIFY_CHUNK = 16;
static u64 g_bitmap_bytes = 0;
static u8* g_verify_scratch = nullptr;

static void release_runtime() {
    g_mounted = false;
    idx_drop_all();
    bloom_drop_all();
    free_ram(g_bitmap, g_bitmap_bytes);
    free_ram(g_bm_dirty, g_bitmap_bytes / SECTOR_SIZE);
    free_ram(g_verify_scratch, VERIFY_CHUNK * SECTOR_SIZE);
    g_bitmap = nullptr;
    g_bm_dirty = nullptr;
    g_verify_scratch = nullptr;
    g_bitmap_bytes = 0;
    g_bm_lo = ~0ull;
    g_bm_hi = 0;
}

static bool alloc_runtime(u64 bitmap_sectors) {
    release_runtime();
    g_bitmap_bytes = bitmap_sectors * SECTOR_SIZE;
    g_bitmap = static_cast<u8*>(alloc_ram(g_bitmap_bytes));
    g_bm_dirty = static_cast<u8*>(alloc_ram(bitmap_sectors));
    if (!g_bitmap || !g_bm_dirty) {
        release_runtime();
        serial::writeln("[stellar] out of memory for the free-space bitmap");
        return false;
    }
    return true;
}

static bool raw_wr(u64 sector, const void* buf) {
    ++g_io_writes;
    if (!blockdev::write_sector(sector, buf)) return fail(Status::Io);
    return true;
}

static u32 sector_crc(const u8* b) { return crc32_full(b, CRC_OFF); }
static bool crc_ok(const u8* b) {
    u32 stored;
    __builtin_memcpy(&stored, b + CRC_OFF, sizeof(stored));
    return stored == sector_crc(b);
}
static void crc_stamp(u8* b) {
    u32 c = sector_crc(b);
    __builtin_memcpy(b + CRC_OFF, &c, sizeof(c));
}
static void cache_drop(u64 first, u64 count);

static bool rd(u64 sector, void* out) {
    CacheSlot& c = g_cache[sector % CACHE_SLOTS];
    if (c.valid && c.sector == sector) {
        ++g_cache_hits;
        __builtin_memcpy(out, c.data, SECTOR_SIZE);
        return true;
    }
    ++g_io_reads;
    if (!blockdev::read_sector(sector, c.data)) { c.valid = false; return fail(Status::Io); }
    if (!crc_ok(c.data)) {
        c.valid = false;
        serial::printf("[stellar] metadata checksum mismatch at sector %lu\n", sector);
        return fail(Status::Checksum);
    }
    c.sector = sector;
    c.valid = true;
    __builtin_memcpy(out, c.data, SECTOR_SIZE);
    return true;
}

static bool wr(u64 sector, const void* in) {
    CacheSlot& c = g_cache[sector % CACHE_SLOTS];
    ++g_io_writes;
    u8 tmp[SECTOR_SIZE];
    __builtin_memcpy(tmp, in, SECTOR_SIZE);
    crc_stamp(tmp);
    if (!blockdev::write_sector(sector, tmp)) {
        if (c.valid && c.sector == sector) c.valid = false;
        return fail(Status::Io);
    }
    c.sector = sector;
    c.valid = true;
    __builtin_memcpy(c.data, tmp, SECTOR_SIZE);
    return true;
}

static bool rd_data(u64 sector, void* out) {
    ++g_io_reads;
    if (!blockdev::read_sector(sector, out)) return fail(Status::Io);
    return true;
}

static bool wr_data(u64 sector, const void* in) {
    ++g_io_writes;
    cache_drop(sector, 1);
    if (!blockdev::write_sector(sector, in)) return fail(Status::Io);
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
    u64 idx = (s / 8) / BM_PAYLOAD;
    g_bm_dirty[idx] = 1;
    if (idx < g_bm_lo) g_bm_lo = idx;
    if (idx > g_bm_hi) g_bm_hi = idx;
}

static bool sb_write_slot(u32 slot) {
    u8 buf[SECTOR_SIZE];
    __builtin_memcpy(buf, &g_sb, sizeof(g_sb));
    crc_stamp(buf);
    return raw_wr(slot, buf);
}

static bool commit() {
    if (!g_mounted) return true;
    bool ok = true;
    if (g_bm_lo <= g_bm_hi) {
        bool all = true;
        u64 total_bytes = (g_sb.total_sectors + 7) / 8;
        for (u64 i = g_bm_lo; i <= g_bm_hi; ++i) {
            if (!g_bm_dirty[i]) continue;
            u8 buf[SECTOR_SIZE];
            for (auto& b : buf) b = 0;
            u64 off = i * BM_PAYLOAD;
            u64 n = off < total_bytes ? total_bytes - off : 0;
            if (n > BM_PAYLOAD) n = BM_PAYLOAD;
            __builtin_memcpy(buf, g_bitmap + off, n);
            crc_stamp(buf);
            if (raw_wr(g_sb.bitmap_start + i, buf)) g_bm_dirty[i] = 0;
            else all = false;
        }
        if (all) {
            g_bm_lo = ~0ull;
            g_bm_hi = 0;
        } else {
            ok = false;
        }
    }
    if (g_sb_dirty && ok) {
        ++g_sb.seq;
        if (sb_write_slot(static_cast<u32>(g_sb.seq & 1))) g_sb_dirty = false;
        else { --g_sb.seq; ok = false; }
    }
    return ok;
}

static u32 g_txn_depth = 0;

struct Txn {
    bool finished = false;
    Txn() { ++g_txn_depth; }
    ~Txn() { if (!finished && --g_txn_depth == 0) commit(); }

    bool finish(bool ok) {
        finished = true;
        if (--g_txn_depth != 0) return ok;
        return commit() && ok;
    }
};

void begin_batch() {
    g_lock.lock();
    ++g_txn_depth;
}

bool end_batch(Status* why) {
    if (g_txn_depth == 0 || !g_lock.held_by_me()) {
        if (why) *why = Status::InvalidArgument;
        return false;
    }
    bool ok = true;
    if (--g_txn_depth == 0) ok = commit();
    g_lock.unlock();
    if (why) *why = ok ? Status::Ok : Status::Io;
    return ok;
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
    if (!r) { fail(Status::NoSpace); return 0; }
    for (u64 j = r; j < r + count; ++j) bm_set(j, true);
    g_alloc_hint = r + count;
    return r;
}

static void release_sectors(u64 first, u64 count) {
    for (u64 j = first; j < first + count; ++j) bm_set(j, false);
    if (first < g_alloc_hint) g_alloc_hint = first;
}

u64 total_sectors() {
    Guard guard;
    return g_mounted ? g_sb.total_sectors : 0;
}

u64 free_space_sectors() {
    Guard guard;
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
    if (snap > g_sb.snap_count) return fail(Status::NoSuchSnapshot);
    *root = g_sb.snaps[snap - 1].catalog_root;
    if (*root == 0) return fail(Status::NoSuchSnapshot);
    return true;
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

static int dir_lookup(const StarEntry& d, const char* name, u64* star) {
    u64 sector = d.first_sector;
    u8 buf[SECTOR_SIZE];
    while (sector != 0) {
        if (!rd(sector, buf)) return -1;
        auto* ds = reinterpret_cast<DirSector*>(buf);
        for (u32 i = 0; i < DIR_ENTRIES_PER_SECTOR; ++i) {
            if (ds->entries[i].in_use && names_equal(ds->entries[i].name, name)) {
                *star = ds->entries[i].star;
                return 1;
            }
        }
        sector = ds->next_sector;
    }
    return 0;
}

static u64 g_hash_mask = ~0ull;

static u64 hash_name(const char* n) {
    u64 h = 0xcbf29ce484222325ull;
    for (u32 i = 0; i < NAME_LEN && n[i]; ++i) {
        h ^= static_cast<u8>(n[i]);
        h *= 0x100000001b3ull;
    }
    h ^= h >> 32;
    h *= 0x9E3779B97F4A7C15ull;
    h ^= h >> 29;
    return h & g_hash_mask;
}

static LookupStats g_ls{};
static u64 g_clock = 0;

constexpr u32 IDX_SLOTS = 4;
constexpr u32 IDX_FREE_MAX = 32;
constexpr u32 IDX_MIN_CAP = 64;

struct IdxEntry { u64 hash; u64 star; u64 sector; u32 slot; u32 state; };
struct FreeSlot { u64 sector; u32 slot; u32 pad; };
struct DirIndex {
    bool valid;
    u64 dir, head, stamp, tail;
    IdxEntry* tab;
    u32 cap, count, tombs, nfree;
    FreeSlot freed[IDX_FREE_MAX];
};
static DirIndex g_idx[IDX_SLOTS];

static void idx_release(DirIndex& ix) {
    if (ix.tab) free_ram(ix.tab, static_cast<u64>(ix.cap) * sizeof(IdxEntry));
    ix = DirIndex{};
}
static void idx_drop_all() { for (auto& ix : g_idx) idx_release(ix); }
static void idx_drop(u64 dir) { for (auto& ix : g_idx) if (ix.valid && ix.dir == dir) idx_release(ix); }

static void idx_put_raw(DirIndex& ix, u64 hash, u64 star, u64 sector, u32 slot) {
    u32 i = static_cast<u32>(hash) & (ix.cap - 1);
    while (ix.tab[i].state == 1) i = (i + 1) & (ix.cap - 1);
    if (ix.tab[i].state == 2) --ix.tombs;
    ix.tab[i] = { hash, star, sector, slot, 1 };
    ++ix.count;
}

static bool idx_resize(DirIndex& ix, u32 new_cap) {
    auto* t = static_cast<IdxEntry*>(alloc_ram_soft(static_cast<u64>(new_cap) * sizeof(IdxEntry)));
    if (!t) return false;
    IdxEntry* old = ix.tab;
    u32 old_cap = ix.cap;
    if (new_cap > ix.cap) ++g_ls.index_grows;
    ix.tab = t;
    ix.cap = new_cap;
    ix.count = 0;
    ix.tombs = 0;
    for (u32 i = 0; i < old_cap; ++i)
        if (old[i].state == 1) idx_put_raw(ix, old[i].hash, old[i].star, old[i].sector, old[i].slot);
    if (old) free_ram(old, static_cast<u64>(old_cap) * sizeof(IdxEntry));
    return true;
}

static bool idx_room(DirIndex& ix) {
    if ((static_cast<u64>(ix.count) + ix.tombs + 1) * 4 <= static_cast<u64>(ix.cap) * 3) return true;
    u32 want = ix.cap;
    if ((static_cast<u64>(ix.count) + 1) * 2 > want) want *= 2;
    return idx_resize(ix, want);
}

static bool idx_insert(DirIndex& ix, const char* name, u64 star, u64 sector, u32 slot) {
    if (!idx_room(ix)) return false;
    idx_put_raw(ix, hash_name(name), star, sector, slot);
    return true;
}

static void idx_erase(DirIndex& ix, const char* name, u64 sector, u32 slot) {
    u64 h = hash_name(name);
    u32 i = static_cast<u32>(h) & (ix.cap - 1);
    for (u32 n = 0; n < ix.cap; ++n, i = (i + 1) & (ix.cap - 1)) {
        IdxEntry& e = ix.tab[i];
        if (e.state == 0) return;
        if (e.state == 1 && e.hash == h && e.sector == sector && e.slot == slot) {
            e.state = 2;
            --ix.count;
            ++ix.tombs;
            return;
        }
    }
}

static int idx_find(DirIndex& ix, const char* name, u64* star, u64* sector, u32* slot) {
    u64 h = hash_name(name);
    u32 i = static_cast<u32>(h) & (ix.cap - 1);
    for (u32 n = 0; n < ix.cap; ++n, i = (i + 1) & (ix.cap - 1)) {
        IdxEntry& e = ix.tab[i];
        if (e.state == 0) return 0;
        if (e.state != 1 || e.hash != h) continue;
        u8 buf[SECTOR_SIZE];
        if (!rd(e.sector, buf)) return -1;
        auto* ds = reinterpret_cast<DirSector*>(buf);
        if (e.slot < DIR_ENTRIES_PER_SECTOR && ds->entries[e.slot].in_use &&
            names_equal(ds->entries[e.slot].name, name)) {
            *star = ds->entries[e.slot].star;
            *sector = e.sector;
            *slot = e.slot;
            return 1;
        }
    }
    return 0;
}

static DirIndex* idx_build(u64 dir_star, const StarEntry& e) {
    DirIndex* v = nullptr;
    for (auto& ix : g_idx) if (!ix.valid) { v = &ix; break; }
    if (!v) {
        v = &g_idx[0];
        for (auto& ix : g_idx) if (ix.stamp < v->stamp) v = &ix;
    }
    idx_release(*v);
    u64 est = static_cast<u64>(e.sector_count) * DIR_ENTRIES_PER_SECTOR * 2;
    u32 want = IDX_MIN_CAP;
    while (want < est && want < (1u << 20)) want <<= 1;
    auto* t = static_cast<IdxEntry*>(alloc_ram_soft(static_cast<u64>(want) * sizeof(IdxEntry)));
    if (!t) return nullptr;
    v->tab = t;
    v->cap = want;
    v->dir = dir_star;
    v->head = e.first_sector;

    u64 sector = e.first_sector;
    u8 buf[SECTOR_SIZE];
    for (u64 hops = 0; sector != 0; ++hops) {
        if (hops > (1ull << 24) || !rd(sector, buf)) {
            if (hops > (1ull << 24)) fail(Status::Corrupt);
            idx_release(*v);
            return nullptr;
        }
        auto* ds = reinterpret_cast<DirSector*>(buf);
        for (u32 i = 0; i < DIR_ENTRIES_PER_SECTOR; ++i) {
            if (ds->entries[i].in_use) {
                if (!idx_insert(*v, ds->entries[i].name, ds->entries[i].star, sector, i)) {
                    idx_release(*v);
                    return nullptr;
                }
            } else if (v->nfree < IDX_FREE_MAX) {
                v->freed[v->nfree++] = { sector, i, 0 };
            }
        }
        v->tail = sector;
        sector = ds->next_sector;
    }
    v->valid = true;
    v->stamp = ++g_clock;
    ++g_ls.index_builds;
    return v;
}

static DirIndex* idx_get(u64 dir_star, const StarEntry& e) {
    for (auto& ix : g_idx) {
        if (!ix.valid || ix.dir != dir_star) continue;
        if (ix.head == e.first_sector) { ix.stamp = ++g_clock; return &ix; }
        idx_release(ix);
        break;
    }
    return idx_build(dir_star, e);
}

constexpr u32 BLOOM_SLOTS = 8;
constexpr u64 BLOOM_BITS_PER_ENTRY = 16;
constexpr u64 BLOOM_MAX_BITS = 1ull << 23;

struct Bloom { bool valid; u64 root, dir, stamp, nbits; u8* bits; };
static Bloom g_bloom[BLOOM_SLOTS];

static void bloom_release(Bloom& b) {
    if (b.bits) free_ram(b.bits, b.nbits / 8);
    b = Bloom{};
}
static void bloom_drop_all() { for (auto& b : g_bloom) bloom_release(b); }

static void bloom_positions(u64 h, u64 nbits, u64* p) {
    u64 step = ((h >> 32) ^ (h << 7)) | 1;
    for (u32 i = 0; i < 4; ++i) p[i] = (h + i * step) & (nbits - 1);
}
static void bloom_add(Bloom& b, u64 h) {
    u64 p[4];
    bloom_positions(h, b.nbits, p);
    for (u32 i = 0; i < 4; ++i) b.bits[p[i] / 8] |= static_cast<u8>(1u << (p[i] % 8));
}
static bool bloom_maybe(const Bloom& b, u64 h) {
    u64 p[4];
    bloom_positions(h, b.nbits, p);
    for (u32 i = 0; i < 4; ++i)
        if (!(b.bits[p[i] / 8] & (1u << (p[i] % 8)))) return false;
    return true;
}

static Bloom* bloom_get(u64 root, u64 dir, const StarEntry& e) {
    for (auto& b : g_bloom)
        if (b.valid && b.root == root && b.dir == dir) { b.stamp = ++g_clock; return &b; }
    Bloom* v = nullptr;
    for (auto& b : g_bloom) if (!b.valid) { v = &b; break; }
    if (!v) {
        v = &g_bloom[0];
        for (auto& b : g_bloom) if (b.stamp < v->stamp) v = &b;
    }
    bloom_release(*v);
    u64 entries = static_cast<u64>(e.sector_count) * DIR_ENTRIES_PER_SECTOR;
    u64 nbits = 256;
    while (nbits < entries * BLOOM_BITS_PER_ENTRY && nbits < BLOOM_MAX_BITS) nbits <<= 1;
    u8* bits = static_cast<u8*>(alloc_ram_soft(nbits / 8));
    if (!bits) return nullptr;
    v->bits = bits;
    v->nbits = nbits;
    u64 sector = e.first_sector;
    u8 buf[SECTOR_SIZE];
    for (u64 hops = 0; sector != 0; ++hops) {
        if (hops > (1ull << 24) || !rd(sector, buf)) { bloom_release(*v); return nullptr; }
        auto* ds = reinterpret_cast<DirSector*>(buf);
        for (u32 i = 0; i < DIR_ENTRIES_PER_SECTOR; ++i)
            if (ds->entries[i].in_use) bloom_add(*v, hash_name(ds->entries[i].name));
        sector = ds->next_sector;
    }
    v->root = root;
    v->dir = dir;
    v->valid = true;
    v->stamp = ++g_clock;
    ++g_ls.bloom_builds;
    return v;
}

static int snapshot_lookup(u64 root, u64 dir_star, const StarEntry& e, const char* name, u64* star) {
    Bloom* b = bloom_get(root, dir_star, e);
    if (b) {
        if (!bloom_maybe(*b, hash_name(name))) { ++g_ls.bloom_rejects; return 0; }
        ++g_ls.bloom_passes;
    }
    ++g_ls.scan_lookups;
    int r = dir_lookup(e, name, star);
    if (r == 0 && b) ++g_ls.bloom_false_positives;
    return r;
}

static int live_lookup(u64 dir_star, const StarEntry& d, const char* name, u64* star) {
    DirIndex* ix = idx_get(dir_star, d);
    if (!ix) { ++g_ls.scan_lookups; return dir_lookup(d, name, star); }
    ++g_ls.index_lookups;
    u64 sector; u32 slot;
    return idx_find(*ix, name, star, &sector, &slot);
}

static bool can_add(u64 parent, const char* name) {
    if (!valid_name(name)) return fail(Status::InvalidName);
    StarEntry d;
    if (!catalog_find(parent, &d)) return fail(Status::NotFound);
    if (d.type != TYPE_CONSTELLATION) return fail(Status::NotADirectory);
    u64 existing;
    int r = live_lookup(parent, d, name, &existing);
    if (r < 0) return false;
    if (r == 1) return fail(Status::Exists);
    return true;
}

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
        if (!wr_data(start + i, buf)) return false;
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
    idx_drop_all();
    bloom_drop_all();
    g_hint_dir = INVALID_STAR;
    g_hint_sector = 0;
    g_hint_epoch = 0;
    g_txn_depth = 0;
}

static bool add_edge_scan(u64 dir_star, const char* name, u64 target, StarEntry& e) {

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

static bool add_edge(u64 dir_star, const char* name, u64 target) {
    StarEntry e;
    if (!catalog_find(dir_star, &e)) return fail(Status::NotFound);
    if (e.type != TYPE_CONSTELLATION) return fail(Status::NotADirectory);
    if (!dir_make_current(e, dir_star)) return false;
    DirIndex* ix = idx_get(dir_star, e);
    if (!ix) return add_edge_scan(dir_star, name, target, e);
    g_hint_dir = INVALID_STAR;

    u8 buf[SECTOR_SIZE];
    u64 sector = 0;
    u32 slot = 0;
    bool placed = false;
    while (ix->nfree > 0 && !placed) {
        FreeSlot f = ix->freed[--ix->nfree];
        if (!rd(f.sector, buf)) { idx_release(*ix); return false; }
        auto* ds = reinterpret_cast<DirSector*>(buf);
        if (f.slot < DIR_ENTRIES_PER_SECTOR && !ds->entries[f.slot].in_use) { sector = f.sector; slot = f.slot; placed = true; }
    }
    if (!placed) {
        if (!rd(ix->tail, buf)) { idx_release(*ix); return false; }
        auto* ds = reinterpret_cast<DirSector*>(buf);
        if (ds->next_sector != 0) { idx_release(*ix); return add_edge_scan(dir_star, name, target, e); }
        for (u32 i = 0; i < DIR_ENTRIES_PER_SECTOR && !placed; ++i)
            if (!ds->entries[i].in_use) { sector = ix->tail; slot = i; placed = true; }
        if (!placed) {
            u64 ns = alloc_sectors(1);
            if (ns == 0) return false;
            u8 zero[SECTOR_SIZE];
            for (auto& b : zero) b = 0;
            reinterpret_cast<DirSector*>(zero)->gen = g_sb.epoch;
            if (!wr(ns, zero)) return false;
            ds->next_sector = ns;
            if (!wr(ix->tail, buf)) { idx_release(*ix); return false; }
            e.sector_count += 1;
            if (!catalog_upsert(dir_star, e)) { idx_release(*ix); return false; }
            ix->tail = ns;
            sector = ns;
            slot = 0;
            placed = true;
        }
    }
    if (!rd(sector, buf)) { idx_release(*ix); return false; }
    auto* ds = reinterpret_cast<DirSector*>(buf);
    ds->entries[slot].in_use = 1;
    ds->entries[slot].star = target;
    copy_name(ds->entries[slot].name, name);
    if (!wr(sector, buf)) { idx_release(*ix); return false; }
    if (!idx_insert(*ix, ds->entries[slot].name, target, sector, slot)) idx_release(*ix);
    return true;
}

static u64 bitmap_sectors_for(u64 total_sectors) {
    u64 bytes = (total_sectors + 7) / 8;
    u64 n = (bytes + BM_PAYLOAD - 1) / BM_PAYLOAD;
    return n == 0 ? 1 : n;
}

static const char* superblock_problem(const Superblock& sb, u64 device_sectors) {
    if (sb.sector_size != SECTOR_SIZE) return "sector size is not 512";
    if (sb.total_sectors < MIN_SECTORS) return "filesystem is too small";
    if (sb.total_sectors > device_sectors) return "filesystem is larger than the device";
    if (sb.bitmap_start != SB_SLOTS) return "bitmap does not follow the superblocks";
    if (sb.bitmap_sectors != bitmap_sectors_for(sb.total_sectors)) return "bitmap size does not match the disk size";
    u64 data_start = sb.bitmap_start + sb.bitmap_sectors;
    if (data_start >= sb.total_sectors) return "no data area";
    if (sb.epoch == 0) return "epoch is zero";
    if (sb.snap_count > SNAPSHOT_MAX) return "snapshot count exceeds the table";
    if (sb.catalog_root < data_start || sb.catalog_root >= sb.total_sectors) return "catalog root is outside the data area";
    for (u32 i = 0; i < sb.snap_count; ++i) {
        u64 root = sb.snaps[i].catalog_root;
        if (root != 0 && (root < data_start || root >= sb.total_sectors)) return "snapshot root is outside the data area";
    }
    return nullptr;
}

static bool format_impl(u64 total_sectors) {
    if (g_txn_depth != 0) return fail(Status::Busy);
    if (total_sectors < MIN_SECTORS || total_sectors > blockdev::capacity_sectors()) return fail(Status::InvalidArgument);
    for (auto& c : g_cache) c.valid = false;
    g_sb = Superblock{};
    g_sb.magic = MAGIC;
    g_sb.version = VERSION;
    g_sb.sector_size = SECTOR_SIZE;
    g_sb.total_sectors = total_sectors;
    g_sb.bitmap_start = SB_SLOTS;
    g_sb.bitmap_sectors = bitmap_sectors_for(total_sectors);
    g_sb.seq = 0;
    g_sb.features_incompat = 0;
    g_sb.features_compat = 0;
    g_sb.next_star_id = 0;
    g_sb.epoch = 1;
    g_sb.snap_count = 0;

    if (!alloc_runtime(g_sb.bitmap_sectors)) return false;
    for (u64 i = 0; i < g_sb.bitmap_sectors; ++i) g_bm_dirty[i] = 1;
    g_bm_lo = 0;
    g_bm_hi = g_sb.bitmap_sectors - 1;
    g_alloc_hint = g_sb.bitmap_start + g_sb.bitmap_sectors;
    g_sb_dirty = true;
    g_mounted = true;
    reset_runtime_state();

    u64 root_sector = alloc_sectors(1);
    if (root_sector == 0) { release_runtime(); return false; }
    u8 buf[SECTOR_SIZE];
    for (auto& b : buf) b = 0;
    auto* leaf = reinterpret_cast<LeafNode*>(buf);
    leaf->is_leaf = 1;
    leaf->gen = g_sb.epoch;
    if (!wr(root_sector, buf)) { release_runtime(); return false; }
    g_sb.catalog_root = root_sector;
    if (!commit()) {
        serial::writeln("[stellar] format: could not write the initial metadata");
        release_runtime();
        return false;
    }
    if (!sb_write_slot(0)) {
        serial::writeln("[stellar] format: could not write the second superblock");
        release_runtime();
        return false;
    }

    u64 root = create_constellation(INVALID_STAR, "");
    if (root != ROOT_STAR) {
        serial::writeln("[stellar] format: root did not land at star 0");
        release_runtime();
        return false;
    }
    serial::printf("[stellar] formatted: %lu sectors, COW B+tree catalog (leaf fanout %u, internal fanout %u), directory fanout %u/sector, O(1) epoch snapshots (max %u)\n",
                    total_sectors, LEAF_MAX, INTERNAL_MAX, DIR_ENTRIES_PER_SECTOR, SNAPSHOT_MAX);
    return true;
}

bool format(u64 total_sectors, Status* why) {
    Api api(why);
    return api.ok(format_impl(total_sectors));
}

enum class SbState { Valid, Blank, Invalid, Unsupported, IoError };

static SbState sb_parse(u32 slot, Superblock* out, const char** why) {
    u8 buf[SECTOR_SIZE];
    ++g_io_reads;
    if (!blockdev::read_sector(slot, buf)) { *why = "unreadable"; return SbState::IoError; }
    Superblock sb;
    __builtin_memcpy(&sb, buf, sizeof(sb));
    if (sb.magic != MAGIC || sb.version != VERSION) { *why = "not a v6 superblock"; return SbState::Blank; }
    if (!crc_ok(buf)) { *why = "bad checksum"; return SbState::Invalid; }
    if (sb.features_incompat & ~FEATURES_INCOMPAT_KNOWN) { *why = "unknown incompatible features"; return SbState::Unsupported; }
    const char* problem = superblock_problem(sb, blockdev::capacity_sectors());
    if (problem) { *why = problem; return SbState::Invalid; }
    *out = sb;
    return SbState::Valid;
}

static bool load_durable() {
    g_mounted = false;
    for (auto& c : g_cache) c.valid = false;
    Superblock best{};
    bool have = false, io_err = false, unsupported = false, invalid = false;
    for (u32 slot = 0; slot < SB_SLOTS; ++slot) {
        Superblock sb;
        const char* why = "";
        switch (sb_parse(slot, &sb, &why)) {
            case SbState::Valid:
                if (!have || sb.seq > best.seq) { best = sb; have = true; }
                break;
            case SbState::IoError: io_err = true; break;
            case SbState::Unsupported: unsupported = true; break;
            case SbState::Invalid:
                invalid = true;
                serial::printf("[stellar] mount: superblock %u rejected, %s\n", slot, why);
                break;
            case SbState::Blank: break;
        }
    }
    if (!have) {
        if (io_err) return fail(Status::Io);
        if (unsupported) return fail(Status::Unsupported);
        if (invalid) return fail(Status::Corrupt);
        serial::writeln("[stellar] mount: bad magic or version, not formatted");
        return fail(Status::NotFormatted);
    }
    g_sb = best;
    if (!alloc_runtime(g_sb.bitmap_sectors)) return false;
    u64 total_bytes = (g_sb.total_sectors + 7) / 8;
    for (u64 i = 0; i < g_sb.bitmap_sectors; ++i) {
        u8 buf[SECTOR_SIZE];
        ++g_io_reads;
        if (!blockdev::read_sector(g_sb.bitmap_start + i, buf)) {
            serial::printf("[stellar] mount: could not read bitmap sector %lu\n", g_sb.bitmap_start + i);
            release_runtime();
            return fail(Status::Io);
        }
        if (!crc_ok(buf)) {
            serial::printf("[stellar] mount: bitmap sector %lu failed its checksum\n", g_sb.bitmap_start + i);
            release_runtime();
            return fail(Status::Checksum);
        }
        u64 off = i * BM_PAYLOAD;
        u64 n = off < total_bytes ? total_bytes - off : 0;
        if (n > BM_PAYLOAD) n = BM_PAYLOAD;
        __builtin_memcpy(g_bitmap + off, buf, n);
    }
    g_alloc_hint = g_sb.bitmap_start + g_sb.bitmap_sectors;
    g_sb_dirty = false;
    g_mounted = true;
    reset_runtime_state();
    return true;
}

static bool mount_impl() {
    if (g_txn_depth != 0) return fail(Status::Busy);
    if (!load_durable()) return false;
    serial::printf("[stellar] mounted: %lu sectors, catalog root at sector %lu, epoch %lu, commit %lu, %u snapshot(s), next star id %lu\n",
                    g_sb.total_sectors, g_sb.catalog_root, g_sb.epoch, g_sb.seq, g_sb.snap_count, g_sb.next_star_id);
    return true;
}

bool mount(Status* why) {
    Api api(why);
    return api.ok(mount_impl());
}

static u64 create_constellation_impl(u64 parent, const char* name) {
    if (!g_mounted) return fail_id(Status::NotMounted);
    Txn txn;
    if (parent != INVALID_STAR && !can_add(parent, name)) return INVALID_STAR;
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
    entry.mtime = now();
    if (!catalog_upsert(id, entry)) return INVALID_STAR;

    if (parent != INVALID_STAR && !add_edge(parent, name, id)) return INVALID_STAR;
    return txn.finish(true) ? id : INVALID_STAR;
}

u64 create_constellation(u64 parent, const char* name, Status* why) {
    Api api(why);
    return api.val(create_constellation_impl(parent, name));
}

static u64 create_file_impl(u64 parent, const char* name, const void* data, u64 size) {
    if (!g_mounted) return fail_id(Status::NotMounted);
    Txn txn;
    if (parent != INVALID_STAR && !can_add(parent, name)) return INVALID_STAR;
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
    entry.mtime = now();
    entry.checksum = crc32_full(static_cast<const u8*>(data), size);
    if (!catalog_upsert(id, entry)) return INVALID_STAR;

    if (parent != INVALID_STAR && !add_edge(parent, name, id)) return INVALID_STAR;
    return txn.finish(true) ? id : INVALID_STAR;
}

u64 create_file(u64 parent, const char* name, const void* data, u64 size, Status* why) {
    Api api(why);
    return api.val(create_file_impl(parent, name, data, size));
}

static u64 write_file_impl(u64 star, const void* data, u64 size) {
    if (!g_mounted) return fail_id(Status::NotMounted);
    Txn txn;
    StarEntry old_e;
    if (!catalog_find(star, &old_e)) return fail_id(Status::NotFound);
    if (old_e.type == TYPE_CONSTELLATION) return fail_id(Status::IsADirectory);
    if (old_e.type != TYPE_FILE) return fail_id(Status::NotFound);

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
    new_e.mtime = now();
    new_e.flags = old_e.flags;
    new_e.checksum = crc32_full(static_cast<const u8*>(data), size);

    if (!catalog_upsert(star, new_e)) return INVALID_STAR;
    if (old_e.gen == g_sb.epoch) release_sectors(old_e.first_sector, old_e.sector_count);
    return txn.finish(true) ? star : INVALID_STAR;
}

u64 write_file(u64 star, const void* data, u64 size, Status* why) {
    Api api(why);
    return api.val(write_file_impl(star, data, size));
}

static u64 snapshot_impl() {
    if (!g_mounted) return fail_id(Status::NotMounted);
    u32 slot = g_sb.snap_count;
    for (u32 i = 0; i < g_sb.snap_count; ++i) {
        if (g_sb.snaps[i].catalog_root == 0) { slot = i; break; }
    }
    if (slot >= SNAPSHOT_MAX) return fail_id(Status::TooManySnapshots);
    Txn txn;
    g_sb.snaps[slot].catalog_root = g_sb.catalog_root;
    g_sb.snaps[slot].epoch = g_sb.epoch;
    if (slot == g_sb.snap_count) ++g_sb.snap_count;
    ++g_sb.epoch;
    g_sb_dirty = true;
    return txn.finish(true) ? static_cast<u64>(slot) + 1 : INVALID_STAR;
}

u64 snapshot(Status* why) {
    Api api(why);
    return api.val(snapshot_impl());
}

static bool link_impl(u64 dir_star, const char* name, u64 target) {
    if (!g_mounted) return fail(Status::NotMounted);
    if (dir_star == target) return fail(Status::InvalidArgument);
    Txn txn;
    StarEntry e;
    if (!catalog_find(target, &e) || e.type == TYPE_FREE) return fail(Status::NotFound);
    if (e.type != TYPE_FILE) return fail(Status::IsADirectory);
    if (!can_add(dir_star, name)) return false;
    if (!add_edge(dir_star, name, target)) return false;
    if (!catalog_find(target, &e)) return false;
    ++e.nlink;
    return txn.finish(catalog_upsert(target, e));
}

bool link(u64 dir_star, const char* name, u64 target, Status* why) {
    Api api(why);
    return api.ok(link_impl(dir_star, name, target));
}

u32 live_snapshot_count() {
    Guard guard;
    u32 n = 0;
    for (u32 i = 0; i < g_sb.snap_count; ++i)
        if (g_sb.snaps[i].catalog_root != 0) ++n;
    return n;
}

static bool delete_snapshot_impl(u64 snap) {
    if (!g_mounted) return fail(Status::NotMounted);
    if (snap == LIVE) return fail(Status::InvalidArgument);
    if (snap > g_sb.snap_count) return fail(Status::NoSuchSnapshot);
    if (g_sb.snaps[snap - 1].catalog_root == 0) return fail(Status::NoSuchSnapshot);
    Txn txn;
    bloom_drop_all();
    g_sb.snaps[snap - 1].catalog_root = 0;
    g_sb.snaps[snap - 1].epoch = 0;
    while (g_sb.snap_count > 0 && g_sb.snaps[g_sb.snap_count - 1].catalog_root == 0)
        --g_sb.snap_count;
    g_sb_dirty = true;
    return txn.finish(true);
}

bool delete_snapshot(u64 snap, Status* why) {
    Api api(why);
    return api.ok(delete_snapshot_impl(snap));
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

static bool remove_edge_scan(const char* name, u64* target_out, StarEntry& e) {

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

static bool remove_edge(u64 dir_star, const char* name, u64* target_out) {
    StarEntry e;
    if (!catalog_find(dir_star, &e)) return fail(Status::NotFound);
    if (e.type != TYPE_CONSTELLATION) return fail(Status::NotADirectory);
    if (!dir_make_current(e, dir_star)) return false;
    DirIndex* ix = idx_get(dir_star, e);
    if (!ix) return remove_edge_scan(name, target_out, e);

    u64 star, sector;
    u32 slot;
    int r = idx_find(*ix, name, &star, &sector, &slot);
    if (r < 0) { idx_release(*ix); return false; }
    if (r == 0) return fail(Status::NotFound);
    u8 buf[SECTOR_SIZE];
    if (!rd(sector, buf)) { idx_release(*ix); return false; }
    auto* ds = reinterpret_cast<DirSector*>(buf);
    ds->entries[slot].in_use = 0;
    ds->entries[slot].star = 0;
    ds->entries[slot].name[0] = 0;
    if (!wr(sector, buf)) { idx_release(*ix); return false; }
    g_hint_dir = INVALID_STAR;
    idx_erase(*ix, name, sector, slot);
    if (ix->nfree < IDX_FREE_MAX) ix->freed[ix->nfree++] = { sector, slot, 0 };
    *target_out = star;
    return true;
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

static bool unlink_impl(u64 dir_star, const char* name) {
    if (!g_mounted) return fail(Status::NotMounted);
    Txn txn;
    u64 target = find(dir_star, name);
    if (target == INVALID_STAR) return false;
    if (target == ROOT_STAR) return fail(Status::InvalidArgument);

    StarEntry t;
    if (!catalog_find(target, &t)) return fail(Status::NotFound);
    if (t.type == TYPE_CONSTELLATION && !dir_is_empty(t)) return fail(Status::NotEmpty);

    u64 removed = INVALID_STAR;
    if (!remove_edge(dir_star, name, &removed) || removed != target) return false;

    if (!catalog_find(target, &t)) return fail(Status::NotFound);
    if (t.nlink > 1) {
        --t.nlink;
        return txn.finish(catalog_upsert(target, t));
    }
    StarEntry dead = t;
    dead.type = TYPE_FREE;
    dead.nlink = 0;
    dead.size_bytes = 0;
    dead.first_sector = 0;
    dead.sector_count = 0;
    dead.checksum = 0;
    if (!catalog_upsert(target, dead)) return false;
    if (t.type == TYPE_CONSTELLATION) idx_drop(target);
    release_current_epoch(t);
    return txn.finish(true);
}

bool unlink(u64 dir_star, const char* name, Status* why) {
    Api api(why);
    return api.ok(unlink_impl(dir_star, name));
}

static u8* g_mark = nullptr;

static bool mk_test(u64 s) { return ((g_mark[s / 8] >> (s % 8)) & 1u) != 0; }
static void mk_set(u64 s) { g_mark[s / 8] |= static_cast<u8>(1u << (s % 8)); }
static void mk_range(u64 first, u64 count) {
    for (u64 s = first; s < first + count && s < g_sb.total_sectors; ++s) mk_set(s);
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

static u64 gc_impl() {
    if (!g_mounted) return fail_id(Status::NotMounted);
    Txn txn;
    u64 bm_bytes = g_sb.bitmap_sectors * SECTOR_SIZE;
    u64 data_start = g_sb.bitmap_start + g_sb.bitmap_sectors;
    g_mark = static_cast<u8*>(alloc_ram(bm_bytes));
    if (!g_mark) return INVALID_STAR;
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
            u8* cur = g_bitmap + i * BM_PAYLOAD;
            const u8* want = g_mark + i * BM_PAYLOAD;
            if (__builtin_memcmp(cur, want, BM_PAYLOAD) == 0) continue;
            __builtin_memcpy(cur, want, BM_PAYLOAD);
            g_bm_dirty[i] = 1;
            if (i < g_bm_lo) g_bm_lo = i;
            if (i > g_bm_hi) g_bm_hi = i;
        }
        g_alloc_hint = data_start;
    }

    free_ram(g_mark, bm_bytes);
    g_mark = nullptr;
    if (freed == INVALID_STAR) return fail_id(Status::Corrupt);
    return txn.finish(true) ? freed : INVALID_STAR;
}

u64 gc(Status* why) {
    Api api(why);
    return api.val(gc_impl());
}

static bool verify_file_impl(u64 star, u64 snap) {
    if (!g_mounted) return fail(Status::NotMounted);
    u64 root;
    if (!view_root(snap, &root)) return false;
    StarEntry e;
    if (!bt_search(root, star, &e)) return fail(Status::NotFound);
    if (e.type == TYPE_CONSTELLATION) return fail(Status::IsADirectory);
    if (e.type != TYPE_FILE) return fail(Status::NotFound);

    constexpr u64 CHUNK = VERIFY_CHUNK;
    if (!g_verify_scratch) g_verify_scratch = static_cast<u8*>(alloc_ram(CHUNK * SECTOR_SIZE));
    if (!g_verify_scratch) return false;
    u8* scratch = g_verify_scratch;

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
        return fail(Status::Checksum);
    }
    return true;
}

bool verify_file(u64 star, u64 snap, Status* why) {
    Api api(why);
    return api.ok(verify_file_impl(star, snap));
}

static u64 read_file_impl(u64 star, void* buf, u64 max_size, u64 snap) {
    if (!g_mounted) { fail(Status::NotMounted); return READ_ERROR; }
    u64 root;
    if (!view_root(snap, &root)) return READ_ERROR;
    StarEntry e;
    if (!bt_search(root, star, &e)) { fail(Status::NotFound); return READ_ERROR; }
    if (e.type == TYPE_CONSTELLATION) { fail(Status::IsADirectory); return READ_ERROR; }
    if (e.type != TYPE_FILE) { fail(Status::NotFound); return READ_ERROR; }
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
        if (!rd_data(e.first_sector + i, sector_buf)) break;
        u64 chunk = to_read - read_so_far;
        if (chunk > SECTOR_SIZE) chunk = SECTOR_SIZE;
        for (u64 b = 0; b < chunk; ++b) dst[read_so_far + b] = sector_buf[b];
        read_so_far += chunk;
    }
    if (read_so_far < to_read) return READ_ERROR;
    if (read_so_far == size && crc32_full(dst, size) != e.checksum) {
        serial::printf("[stellar] checksum mismatch reading star %lu, refusing the data\n", star);
        fail(Status::Checksum);
        return READ_ERROR;
    }
    return read_so_far;
}

u64 read_file(u64 star, void* buf, u64 max_size, u64 snap, Status* why) {
    Api api(why);
    return api.val(read_file_impl(star, buf, max_size, snap));
}

static u64 find_impl(u64 dir_star, const char* name, u64 snap) {
    if (!g_mounted) return fail_id(Status::NotMounted);
    if (!valid_name(name)) return fail_id(Status::InvalidName);
    u64 root;
    if (!view_root(snap, &root)) return INVALID_STAR;
    StarEntry e;
    if (!bt_search(root, dir_star, &e)) return fail_id(Status::NotFound);
    if (e.type != TYPE_CONSTELLATION) return fail_id(Status::NotADirectory);
    u64 star = INVALID_STAR;
    int r = snap == LIVE ? live_lookup(dir_star, e, name, &star)
                         : snapshot_lookup(root, dir_star, e, name, &star);
    if (r < 0) return INVALID_STAR;
    if (r == 0) return fail_id(Status::NotFound);
    return star;
}

u64 find(u64 dir_star, const char* name, u64 snap, Status* why) {
    Api api(why);
    return api.val(find_impl(dir_star, name, snap));
}

static void list_impl(u64 dir_star, ListCallback cb, void* ctx, u64 snap) {
    if (!g_mounted) { fail(Status::NotMounted); return; }
    u64 root;
    if (!view_root(snap, &root)) return;
    StarEntry e;
    if (!bt_search(root, dir_star, &e)) { fail(Status::NotFound); return; }
    if (e.type != TYPE_CONSTELLATION) { fail(Status::NotADirectory); return; }
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

void list(u64 dir_star, ListCallback cb, void* ctx, u64 snap, Status* why) {
    Api api(why);
    list_impl(dir_star, cb, ctx, snap);
    api.report(g_err == Status::Ok);
}

static u64 resolve_n(const char* path, u64 len, u64 snap) {
    if (!g_mounted) return fail_id(Status::NotMounted);
    if (!path || len == 0 || path[0] != '/') return fail_id(Status::InvalidName);
    u64 cur = ROOT_STAR;
    u64 i = 0;
    bool trailing = false;
    while (i < len) {
        while (i < len && path[i] == '/') { ++i; trailing = true; }
        if (i >= len) break;
        trailing = false;
        char comp[NAME_LEN];
        u32 n = 0;
        while (i < len && path[i] != '/') {
            if (n >= NAME_LEN - 1) return fail_id(Status::InvalidName);
            comp[n++] = path[i++];
        }
        comp[n] = 0;
        if (!valid_name(comp)) return fail_id(Status::InvalidName);
        u64 next = find_impl(cur, comp, snap);
        if (next == INVALID_STAR) return INVALID_STAR;
        cur = next;
    }
    if (trailing && cur != ROOT_STAR) {
        u64 root;
        StarEntry e;
        if (!view_root(snap, &root)) return INVALID_STAR;
        if (!bt_search(root, cur, &e)) return fail_id(Status::NotFound);
        if (e.type != TYPE_CONSTELLATION) return fail_id(Status::NotADirectory);
    }
    return cur;
}

u64 resolve(const char* path, u64 snap, Status* why) {
    Api api(why);
    u64 len = path ? strlen(path) : 0;
    return api.val(resolve_n(path, len, snap));
}

static u64 resolve_parent_impl(const char* path, char* leaf, u64 leaf_size, u64 snap) {
    if (!path || !leaf || path[0] != '/') return fail_id(Status::InvalidName);
    u64 len = strlen(path);
    while (len > 1 && path[len - 1] == '/') --len;
    u64 slash = len;
    while (slash > 0 && path[slash - 1] != '/') --slash;
    u64 leaf_len = len - slash;
    if (leaf_len == 0 || leaf_len >= NAME_LEN || leaf_len >= leaf_size) return fail_id(Status::InvalidName);
    char comp[NAME_LEN];
    for (u64 k = 0; k < leaf_len; ++k) comp[k] = path[slash + k];
    comp[leaf_len] = 0;
    if (!valid_name(comp)) return fail_id(Status::InvalidName);
    u64 parent = resolve_n(path, slash, snap);
    if (parent == INVALID_STAR) return INVALID_STAR;
    for (u64 k = 0; k <= leaf_len; ++k) leaf[k] = comp[k];
    return parent;
}

u64 resolve_parent(const char* path, char* leaf, u64 leaf_size, u64 snap, Status* why) {
    Api api(why);
    return api.val(resolve_parent_impl(path, leaf, leaf_size, snap));
}

static bool stat_impl(u64 star, StatInfo* out, u64 snap) {
    if (!g_mounted) return fail(Status::NotMounted);
    if (!out) return fail(Status::InvalidArgument);
    u64 root;
    if (!view_root(snap, &root)) return false;
    StarEntry e;
    if (!bt_search(root, star, &e) || e.type == TYPE_FREE) return fail(Status::NotFound);
    *out = { e.type, e.nlink, e.flags, e.sector_count, e.size_bytes, e.mtime, e.gen };
    return true;
}

bool stat(u64 star, StatInfo* out, u64 snap, Status* why) {
    Api api(why);
    return api.ok(stat_impl(star, out, snap));
}

static bool set_flags_impl(u64 star, u32 flags) {
    if (!g_mounted) return fail(Status::NotMounted);
    if (flags & ~USER_FLAGS_MASK) return fail(Status::InvalidArgument);
    Txn txn;
    StarEntry e;
    if (!catalog_find(star, &e) || e.type == TYPE_FREE) return fail(Status::NotFound);
    e.flags = (e.flags & ~USER_FLAGS_MASK) | flags;
    return txn.finish(catalog_upsert(star, e));
}

bool set_flags(u64 star, u32 flags, Status* why) {
    Api api(why);
    return api.ok(set_flags_impl(star, flags));
}

LookupStats lookup_stats() {
    Guard guard;
    return g_ls;
}

void test_set_hash_mask(u64 mask) {
    Guard guard;
    idx_drop_all();
    bloom_drop_all();
    g_hash_mask = mask ? mask : ~0ull;
}

}
