#include "../userland/pehost/ucrt.hpp"
#include "pe_samples_mingw.hpp"

extern "C" {
int printf(const char*, ...);
int snprintf(char*, unsigned long, const char*, ...);
int strcmp(const char*, const char*);
int strncmp(const char*, const char*, unsigned long);
char* strstr(const char*, const char*);
char* strchr(const char*, int);
char* strrchr(const char*, int);
void* malloc(unsigned long);
void free(void*);
}

namespace {

int g_failures = 0;
bool g_quick = false;

void expect(bool cond, const char* what) {
    printf("%-100s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) ++g_failures;
}

i64 lx(u64 nr, u64 a = 0, u64 b = 0, u64 c = 0, u64 d = 0, u64 e = 0, u64 f = 0) {
    i64 result;
    register u64 r10 asm("r10") = d;
    register u64 r8 asm("r8") = e;
    register u64 r9 asm("r9") = f;
    asm volatile("syscall" : "=a"(result) : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return result;
}

constexpr u64 SYS_READ = 0, SYS_WRITE = 1, SYS_CLOSE = 3, SYS_MMAP = 9, SYS_MUNMAP = 11, SYS_PIPE = 22, SYS_FORK = 57, SYS_WAIT4 = 61, SYS_ARCH_PRCTL = 158,
              SYS_EXIT_GROUP = 231;

u64 g_rng = 0x2545F4914F6CDD1Dull;
u64 rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

u64 below(u64 n) { return rnd() % n; }

u8* map_pages(u64 bytes) {
    i64 r = lx(SYS_MMAP, 0, bytes, 3, 0x22, static_cast<u64>(-1), 0);
    return (r > 0 || r < -4095) ? reinterpret_cast<u8*>(r) : nullptr;
}

bool contains(const char* hay, const char* needle) { return strstr(hay, needle) != nullptr; }

bool same(const char* a, const char* b) { return strcmp(a, b) == 0; }

u32 g_map_calls = 0;
u32 g_map_fail_at = 0;
u64 g_map_skew = 0;
u8* test_map(u64 bytes) {
    ++g_map_calls;
    if (g_map_fail_at && g_map_calls == g_map_fail_at) return nullptr;
    u8* p = map_pages(bytes + 64);
    return p ? p + g_map_skew : nullptr;
}

struct Live {
    u8* p;
    u64 size;
    u8 tag;
};

void paint(const Live& a, u64 from = 0) {
    for (u64 i = from; i < a.size; ++i) a.p[i] = static_cast<u8>(a.tag + i * 7);
}

bool intact(const Live& a, u64 upto) {
    for (u64 i = 0; i < upto && i < a.size; ++i) {
        if (a.p[i] != static_cast<u8>(a.tag + i * 7)) return false;
    }
    return true;
}

u64 pick_size() {
    u64 r = below(100);
    if (r < 60) return below(200);
    if (r < 90) return below(4000);
    if (r < 99) return below(70000);
    return below(900000);
}

void heap_tests() {
    static pehost::Heap h;
    g_map_calls = g_map_fail_at = 0;
    g_map_skew = 0;
    h.init(&test_map);
    expect(h.check() && h.live() == 0 && h.chunks() == 0 && h.mapped() == 0, "a new heap is empty and consistent");

    void* a = h.alloc(1);
    void* b = h.alloc(100);
    void* z = h.alloc(0);
    expect(a && b && z && a != b && b != z && reinterpret_cast<u64>(a) % 16 == 0 && reinterpret_cast<u64>(b) % 16 == 0 && reinterpret_cast<u64>(z) % 16 == 0,
           "allocations are 16 byte aligned, distinct, and a zero byte request still gives a pointer");
    expect(h.usable(a) >= 1 && h.usable(b) >= 100 && h.usable(z) >= 16 && h.chunks() == 1 && h.mapped() == pehost::Heap::FIRST_CHUNK && h.live() == 3 && h.check(),
           "the first chunk is 256 KiB and block sizes cover the request");
    expect(h.usable(a) == 16 && h.usable(b) == 112, "a block is its request rounded up with a 16 byte header, at least 16 usable bytes");
    expect(h.free(b) && !h.free(b) && h.live() == 2 && h.check(), "a double free is refused and changes nothing");
    expect(!h.free(nullptr) && !h.free(&h) && !h.free(static_cast<u8*>(a) + 16) && !h.free(static_cast<u8*>(a) + 1) && !h.free(static_cast<u8*>(a) - 16) && h.live() == 2 && h.check(),
           "null, foreign, interior, misaligned and header pointers are refused");
    expect(h.owns(a) && !h.owns(b) && !h.owns(nullptr) && h.usable(b) == 0 && h.usable(nullptr) == 0, "ownership is only for live blocks");
    h.free(a);
    h.free(z);
    expect(h.live() == 0 && h.check(), "everything freed coalesces back");
    u64 used = 1, spare = 0;
    expect(h.check(&used, &spare) && used == 0 && spare == pehost::Heap::FIRST_CHUNK - 16, "after freeing all, one free block spans the chunk");

    expect(h.alloc(pehost::Heap::CAP + 1) == nullptr && h.alloc(~0ull) == nullptr && h.check(), "a request above the cap or of absurd size fails cleanly");
    void* big = h.alloc(pehost::Heap::FIRST_CHUNK);
    expect(big && h.chunks() == 2 && h.check(), "a request that does not fit the first chunk maps another, sized for it");
    h.free(big);

    Live live[2048];
    u32 count = 0;
    u32 ops = g_quick ? 40000 : 400000;
    u32 failed_allocs = 0, corrupt = 0, bad_check = 0, bad_align = 0;
    for (u32 i = 0; i < ops; ++i) {
        u64 op = below(100);
        if ((op < 45 && count < 2048) || count == 0) {
            u64 size = pick_size();
            u8* p = static_cast<u8*>(h.alloc(size));
            if (!p) {
                ++failed_allocs;
                continue;
            }
            if (reinterpret_cast<u64>(p) % 16 != 0 || h.usable(p) < size) ++bad_align;
            live[count] = Live{ p, size, static_cast<u8>(rnd()) };
            paint(live[count]);
            ++count;
        } else if (op < 75) {
            u32 k = static_cast<u32>(below(count));
            if (!intact(live[k], live[k].size)) ++corrupt;
            if (!h.free(live[k].p)) ++corrupt;
            live[k] = live[--count];
        } else {
            u32 k = static_cast<u32>(below(count));
            u64 size = pick_size();
            if (!intact(live[k], live[k].size)) ++corrupt;
            u8* q = static_cast<u8*>(h.resize(live[k].p, size));
            if (!q) {
                if (!intact(live[k], live[k].size)) ++corrupt;
                continue;
            }
            u64 keep = live[k].size < size ? live[k].size : size;
            Live moved{ q, size, live[k].tag };
            if (!intact(moved, keep)) ++corrupt;
            live[k] = moved;
            paint(live[k], keep);
            if (h.usable(q) < size) ++bad_align;
        }
        if (i % 997 == 0 && !h.check()) ++bad_check;
    }
    bool disjoint = true;
    for (u32 i = 0; i < count && disjoint; ++i) {
        if (!intact(live[i], live[i].size)) ++corrupt;
        for (u32 k = i + 1; k < count; ++k) {
            if (live[i].p < live[k].p + live[k].size && live[k].p < live[i].p + live[i].size) {
                disjoint = false;
                break;
            }
        }
    }
    expect(corrupt == 0 && bad_check == 0 && bad_align == 0 && disjoint && h.live() == count && h.check(),
           "400k random allocate, free and resize operations: contents survive, blocks never overlap, invariants hold");
    for (u32 i = 0; i < count; ++i) h.free(live[i].p);
    expect(h.live() == 0 && h.check(&used, &spare) && used == 0, "freeing the survivors leaves nothing in use");
    expect(h.mapped() <= pehost::Heap::CAP && h.chunks() <= pehost::Heap::MAX_CHUNKS, "the heap stayed inside its budget");

    static pehost::Heap g;
    g_map_calls = g_map_fail_at = 0;
    g.init(&test_map);
    void* held[4096];
    u32 n = 0;
    for (;;) {
        void* p = g.alloc(200000);
        if (!p || n == 4096) break;
        held[n++] = p;
    }
    expect(n > 30 && g.mapped() <= pehost::Heap::CAP && g.check(), "allocating until failure exhausts the budget without exceeding it");
    expect(g.alloc(200000) == nullptr && g.check(), "a heap that has run out refuses further large requests and stays consistent");
    for (u32 i = 0; i < n; ++i) g.free(held[i]);
    expect(g.live() == 0 && g.check(), "and recovers when the blocks come back");
    void* whole = g.alloc(200000);
    expect(whole != nullptr, "freed space is reused");
    g.free(whole);

    static pehost::Heap f;
    g_map_calls = 0;
    g_map_fail_at = 1;
    f.init(&test_map);
    expect(f.alloc(8) == nullptr && f.chunks() == 0 && f.check(), "a failing platform gives a null allocation and no chunk");
    g_map_fail_at = 0;
    g_map_calls = 0;
    g_map_skew = 8;
    expect(f.alloc(8) == nullptr && f.chunks() == 0 && f.check(), "a misaligned chunk from the platform is refused");
    g_map_skew = 0;
    f.init(nullptr);
    expect(f.alloc(8) == nullptr && f.check(), "a heap with no platform fails cleanly");

    static pehost::Heap r;
    g_map_calls = g_map_fail_at = 0;
    r.init(&test_map);
    u8* p = static_cast<u8*>(r.alloc(100));
    u8* q = static_cast<u8*>(r.alloc(100));
    r.alloc(100);
    for (int i = 0; i < 100; ++i) p[i] = static_cast<u8>(i);
    expect(r.resize(p, 110) == p, "growing within the rounding slack keeps the address");
    u8* moved = static_cast<u8*>(r.resize(p, 300));
    bool kept = moved && moved != p;
    for (int i = 0; i < 100 && kept; ++i) kept = moved[i] == i;
    expect(kept && !r.owns(p) && r.check(), "growing past a neighbour in use moves the block, keeps the bytes and frees the old one");
    r.free(q);
    u8* more = static_cast<u8*>(r.resize(moved, 150));
    expect(more == moved && r.check(), "shrinking keeps the address");
    u8* first = static_cast<u8*>(r.alloc(100));
    u8* second = static_cast<u8*>(r.alloc(100));
    u8* guard = static_cast<u8*>(r.alloc(100));
    r.free(second);
    u8* in_place = static_cast<u8*>(r.resize(first, 200));
    expect(in_place == first && r.usable(first) >= 200 && r.check(), "growing into a free neighbour happens in place");
    r.free(guard);
    u8* e1 = static_cast<u8*>(r.alloc(48));
    u8* e2 = static_cast<u8*>(r.alloc(48));
    u8* e3 = static_cast<u8*>(r.alloc(48));
    r.free(e2);
    expect(r.usable(e1) == 48 && r.resize(e1, 112) == e1 && r.usable(e1) == 112 && r.check(), "growing into a free neighbour to exactly its end stays in place");
    r.free(e3);
    r.free(e1);
    u8* small = static_cast<u8*>(r.resize(more, 20));
    expect(small == more && r.usable(small) == 32 && r.check(), "shrinking splits off the tail and keeps the address");
    u8* none = static_cast<u8*>(r.resize(small, 0));
    expect(none == small && r.usable(none) == 32 && r.check(), "resizing to zero keeps the block, because a 16 byte remainder is too small to split off");
    u8* trimmed = static_cast<u8*>(r.resize(none, 100));
    u8* tiny = static_cast<u8*>(r.resize(trimmed, 0));
    expect(tiny == trimmed && r.usable(tiny) == 16 && r.check(), "a larger block resized to zero shrinks to the 16 byte minimum");
    small = tiny;
    expect(r.resize(nullptr, 40) != nullptr && r.resize(&r, 40) == nullptr && r.resize(small, ~0ull) == nullptr && r.owns(small) && r.check(), "resize of null allocates, of a foreign pointer or an absurd size fails and leaves the block");
    u8* gone = static_cast<u8*>(r.alloc(10));
    r.free(gone);
    expect(r.resize(gone, 10) == nullptr && r.check(), "resizing a freed block fails");

    static pehost::Heap c;
    c.init(&test_map);
    u8* x = static_cast<u8*>(c.alloc(64));
    u8* y = static_cast<u8*>(c.alloc(64));
    c.alloc(64);
    u64 saved = *reinterpret_cast<u64*>(x - 16);
    *reinterpret_cast<u64*>(x - 16) = (saved & ~15ull) + 16 * 100 + (saved & 15);
    expect(!c.check(), "a corrupted block size is detected by the consistency check");
    *reinterpret_cast<u64*>(x - 16) = saved;
    expect(c.check(), "and restoring it passes again");
    c.free(y);
    saved = *reinterpret_cast<u64*>(y + 72);
    *reinterpret_cast<u64*>(y + 72) = 7;
    expect(!c.check(), "a corrupted boundary tag is detected");
    *reinterpret_cast<u64*>(y + 72) = saved;
    expect(c.check(), "and restoring it passes again");
    *reinterpret_cast<u64*>(y) = 0;
    expect(!c.check(), "a broken free list link is detected");

    static pehost::Heap k;
    g_map_calls = g_map_fail_at = 0;
    k.init(&test_map);
    u8* blocks[8];
    for (u32 i = 0; i < 8; ++i) blocks[i] = static_cast<u8*>(k.alloc(48));
    auto flags = [](u8* p) -> u64& { return *reinterpret_cast<u64*>(p - 16); };
    u64 keep_flags;
    keep_flags = flags(blocks[2]);
    flags(blocks[2]) = 16 | (keep_flags & 15);
    expect(!k.check(), "a block smaller than the minimum is detected");
    flags(blocks[2]) = keep_flags;
    flags(blocks[2]) = (1ull << 30) | (keep_flags & 15);
    expect(!k.check(), "a block that runs past its chunk is detected");
    flags(blocks[2]) = keep_flags;
    flags(blocks[3]) = keep_flags ^ 2;
    expect(!k.check(), "a wrong previous-in-use flag is detected");
    flags(blocks[3]) = keep_flags;
    expect(k.check(), "all of those restore cleanly");

    u64 b4 = flags(blocks[4]);
    flags(blocks[4]) = b4 & ~1ull;
    expect(!k.check(), "a free block that is not on the list and is not coalesced is detected");
    flags(blocks[4]) = b4;

    u64* sentinel = reinterpret_cast<u64*>(blocks[0] - 16 + pehost::Heap::FIRST_CHUNK - 16);
    u64 sentinel_keep = *sentinel;
    *sentinel = 64 | (sentinel_keep & 15);
    expect(!k.check(), "a sentinel with a size is detected");
    *sentinel = sentinel_keep & ~1ull;
    expect(!k.check(), "a sentinel that is not marked in use is detected");
    *sentinel = sentinel_keep ^ 2;
    expect(!k.check(), "a sentinel with the wrong previous flag is detected");
    *sentinel = sentinel_keep;
    expect(k.check(), "and the original passes");

    k.free(blocks[1]);
    k.free(blocks[3]);
    k.free(blocks[5]);
    expect(k.check(), "three separated free blocks are consistent");
    u64* second_links = reinterpret_cast<u64*>(blocks[3]);
    u64 saved_prev = second_links[1];
    second_links[1] = 0x1000;
    expect(!k.check(), "a free list node with the wrong back pointer is detected");
    second_links[1] = saved_prev;
    static u64 fake[4] = {};
    u64* tail = reinterpret_cast<u64*>(blocks[7] + 48);
    u64 saved_next = second_links[0];
    u64 saved_tail_prev = tail[1];
    second_links[0] = reinterpret_cast<u64>(&fake[0]);
    fake[2] = reinterpret_cast<u64>(tail - 2);
    fake[3] = reinterpret_cast<u64>(blocks[3] - 16);
    tail[1] = reinterpret_cast<u64>(&fake[0]);
    expect(!k.check(), "a free list node outside every chunk is detected even when the count still matches");
    second_links[0] = saved_next;
    tail[1] = saved_tail_prev;
    expect(k.check(), "and everything restores");
}

struct Utf8Case {
    const char* name;
    u8 bytes[8];
    u32 length;
    u16 units[8];
    u32 unit_count;
};

bool oracle_valid_utf8(const u8* s, u64 n) {
    u64 i = 0;
    while (i < n) {
        u8 b = s[i];
        u64 len;
        u8 lo = 0x80, hi = 0xBF;
        if (b <= 0x7F) {
            ++i;
            continue;
        } else if (b >= 0xC2 && b <= 0xDF) {
            len = 2;
        } else if (b == 0xE0) {
            len = 3;
            lo = 0xA0;
        } else if (b == 0xED) {
            len = 3;
            hi = 0x9F;
        } else if (b >= 0xE1 && b <= 0xEF) {
            len = 3;
        } else if (b == 0xF0) {
            len = 4;
            lo = 0x90;
        } else if (b == 0xF4) {
            len = 4;
            hi = 0x8F;
        } else if (b >= 0xF1 && b <= 0xF3) {
            len = 4;
        } else {
            return false;
        }
        if (i + len > n) return false;
        if (s[i + 1] < lo || s[i + 1] > hi) return false;
        for (u64 k = 2; k < len; ++k) {
            if (s[i + k] < 0x80 || s[i + k] > 0xBF) return false;
        }
        i += len;
    }
    return true;
}

u32 oracle_encode(u32 c, u8* out) {
    if (c < 0x80) {
        out[0] = static_cast<u8>(c);
        return 1;
    }
    if (c < 0x800) {
        out[0] = static_cast<u8>(0xC0 | (c >> 6));
        out[1] = static_cast<u8>(0x80 | (c & 63));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = static_cast<u8>(0xE0 | (c >> 12));
        out[1] = static_cast<u8>(0x80 | ((c >> 6) & 63));
        out[2] = static_cast<u8>(0x80 | (c & 63));
        return 3;
    }
    out[0] = static_cast<u8>(0xF0 | (c >> 18));
    out[1] = static_cast<u8>(0x80 | ((c >> 12) & 63));
    out[2] = static_cast<u8>(0x80 | ((c >> 6) & 63));
    out[3] = static_cast<u8>(0x80 | (c & 63));
    return 4;
}

void codec_tests() {
    using namespace pehost::codec;
    u16 w[16];
    u8 b[64];

    u32 mismatched = 0, bad_len = 0, bad_back = 0;
    for (u32 c = 0; c <= 0x10FFFF; ++c) {
        if (c >= 0xD800 && c <= 0xDFFF) continue;
        u8 expected[4];
        u32 el = oracle_encode(c, expected);
        u16 unit[2];
        u32 un;
        if (c >= 0x10000) {
            unit[0] = static_cast<u16>(0xD800 + ((c - 0x10000) >> 10));
            unit[1] = static_cast<u16>(0xDC00 + ((c - 0x10000) & 0x3FF));
            un = 2;
        } else {
            unit[0] = static_cast<u16>(c);
            un = 1;
        }
        i64 n = utf16_to_utf8(unit, un, b, sizeof(b), true);
        if (n != static_cast<i64>(el)) ++bad_len;
        for (u32 i = 0; i < el; ++i) {
            if (b[i] != expected[i]) {
                ++mismatched;
                break;
            }
        }
        i64 m = utf8_to_utf16(expected, el, w, 16, true);
        if (m != static_cast<i64>(un) || w[0] != unit[0] || (un == 2 && w[1] != unit[1])) ++bad_back;
        if (utf16_to_utf8(unit, un, nullptr, 0, true) != static_cast<i64>(el) || utf8_to_utf16(expected, el, nullptr, 0, true) != static_cast<i64>(un)) ++bad_len;
    }
    expect(mismatched == 0 && bad_len == 0 && bad_back == 0, "all 1,112,064 scalar values encode and decode against an independent encoder, and the size queries agree");

    static const Utf8Case cases[] = {
        { "C0 80", { 0xC0, 0x80 }, 2, { 0xFFFD, 0xFFFD }, 2 },
        { "C1 BF", { 0xC1, 0xBF }, 2, { 0xFFFD, 0xFFFD }, 2 },
        { "E0 80 80", { 0xE0, 0x80, 0x80 }, 3, { 0xFFFD, 0xFFFD, 0xFFFD }, 3 },
        { "E0 9F BF", { 0xE0, 0x9F, 0xBF }, 3, { 0xFFFD, 0xFFFD, 0xFFFD }, 3 },
        { "ED A0 80", { 0xED, 0xA0, 0x80 }, 3, { 0xFFFD, 0xFFFD, 0xFFFD }, 3 },
        { "ED 9F BF", { 0xED, 0x9F, 0xBF }, 3, { 0xD7FF }, 1 },
        { "F0 80 80 80", { 0xF0, 0x80, 0x80, 0x80 }, 4, { 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD }, 4 },
        { "F4 90 80 80", { 0xF4, 0x90, 0x80, 0x80 }, 4, { 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD }, 4 },
        { "F5", { 0xF5 }, 1, { 0xFFFD }, 1 },
        { "FF", { 0xFF }, 1, { 0xFFFD }, 1 },
        { "80", { 0x80 }, 1, { 0xFFFD }, 1 },
        { "C2", { 0xC2 }, 1, { 0xFFFD }, 1 },
        { "E2 82", { 0xE2, 0x82 }, 2, { 0xFFFD }, 1 },
        { "F0 90 80", { 0xF0, 0x90, 0x80 }, 3, { 0xFFFD }, 1 },
        { "E2 82 41", { 0xE2, 0x82, 0x41 }, 3, { 0xFFFD, 0x41 }, 2 },
        { "61 80 62", { 0x61, 0x80, 0x62 }, 3, { 0x61, 0xFFFD, 0x62 }, 3 },
        { "C3 A9", { 0xC3, 0xA9 }, 2, { 0xE9 }, 1 },
        { "F4 8F BF BF", { 0xF4, 0x8F, 0xBF, 0xBF }, 4, { 0xDBFF, 0xDFFF }, 2 },
    };
    u32 lenient_ok = 0, strict_ok = 0;
    for (const Utf8Case& c : cases) {
        i64 n = utf8_to_utf16(c.bytes, c.length, w, 16, false);
        bool same_units = n == static_cast<i64>(c.unit_count);
        for (u32 i = 0; i < c.unit_count && same_units; ++i) same_units = w[i] == c.units[i];
        if (same_units) ++lenient_ok;
        else printf("    lenient mismatch for %s (%lld units)\n", c.name, static_cast<long long>(n));
        bool valid = !(c.unit_count == 1 ? c.units[0] == 0xFFFD : false) && oracle_valid_utf8(c.bytes, c.length);
        i64 sn = utf8_to_utf16(c.bytes, c.length, w, 16, true);
        bool strict_matches = valid ? sn == static_cast<i64>(c.unit_count) : sn == INVALID;
        if (strict_matches) ++strict_ok;
        else printf("    strict mismatch for %s\n", c.name);
    }
    expect(lenient_ok == sizeof(cases) / sizeof(cases[0]), "ill-formed UTF-8 is replaced by U+FFFD one maximal subpart at a time");
    expect(strict_ok == sizeof(cases) / sizeof(cases[0]), "strict decoding fails exactly on the ill-formed cases");

    u32 disagree = 0, grew = 0;
    u32 rounds = g_quick ? 100000 : 1000000;
    for (u32 r = 0; r < rounds; ++r) {
        u8 in[12];
        u32 n = static_cast<u32>(below(12));
        for (u32 i = 0; i < n; ++i) {
            u64 pick = below(8);
            in[i] = pick < 3 ? static_cast<u8>(below(0x80)) : pick < 5 ? static_cast<u8>(0x80 + below(0x40)) : static_cast<u8>(0xC0 + below(0x40));
            if (pick == 7) in[i] = static_cast<u8>(0xE0 + below(0x20));
        }
        i64 strict = utf8_to_utf16(in, n, w, 16, true);
        i64 lenient = utf8_to_utf16(in, n, w, 16, false);
        bool valid = oracle_valid_utf8(in, n);
        if (valid != (strict >= 0)) ++disagree;
        if (lenient < 0 || lenient > static_cast<i64>(n)) ++grew;
        if (valid && strict != lenient) ++disagree;
        i64 size_only = utf8_to_utf16(in, n, nullptr, 0, false);
        if (size_only != lenient) ++disagree;
        if (lenient > 0) {
            i64 back = utf16_to_utf8(w, static_cast<u64>(lenient), b, sizeof(b), true);
            if (back < 0 || !oracle_valid_utf8(b, static_cast<u64>(back))) ++disagree;
            if (valid && (back != static_cast<i64>(n) || memcmp(b, in, n) != 0)) ++disagree;
        }
    }
    expect(disagree == 0 && grew == 0, "random byte strings: strict success equals the independent validity check, lenient output is well formed, valid text round-trips byte for byte");

    u8 e[4] = { 0xC3, 0xA9, 0, 0 };
    expect(utf8_to_utf16(e, 2, w, 0, false) == TOO_SMALL && utf8_to_utf16(e, 2, w, 1, false) == 1 && utf8_to_utf16(e, 2, nullptr, 0, false) == 1, "a UTF-16 buffer that is too small is reported, an exact one works");
    u8 astral[4] = { 0xF0, 0x9F, 0x98, 0x80 };
    expect(utf8_to_utf16(astral, 4, w, 1, false) == TOO_SMALL && utf8_to_utf16(astral, 4, w, 2, false) == 2 && w[0] == 0xD83D && w[1] == 0xDE00, "a surrogate pair is never split across the end of the buffer");
    u16 euro = 0x20AC;
    expect(utf16_to_utf8(&euro, 1, b, 2, false) == TOO_SMALL && utf16_to_utf8(&euro, 1, b, 3, false) == 3 && b[0] == 0xE2 && b[1] == 0x82 && b[2] == 0xAC, "a UTF-8 buffer that is too small is reported, an exact one works");
    u16 lone_top = 0xDFFF;
    u8 top_out[8] = {};
    expect(utf16_to_utf8(&lone_top, 1, top_out, 8, true) == INVALID && utf16_to_utf8(&lone_top, 1, top_out, 8, false) == 3 && top_out[0] == 0xEF && top_out[1] == 0xBF && top_out[2] == 0xBD,
           "the last surrogate, U+DFFF, alone is invalid in strict mode and U+FFFD otherwise");
    u16 lone_high = 0xD800, lone_low = 0xDC00;
    u16 swapped[2] = { 0xDC00, 0xD800 };
    u16 high_then_a[2] = { 0xD800, 0x41 };
    expect(utf16_to_utf8(&lone_high, 1, b, 8, true) == INVALID && utf16_to_utf8(&lone_low, 1, b, 8, true) == INVALID && utf16_to_utf8(swapped, 2, b, 8, true) == INVALID &&
               utf16_to_utf8(high_then_a, 2, b, 8, true) == INVALID,
           "lone and misordered surrogates are invalid when strict");
    expect(utf16_to_utf8(&lone_high, 1, b, 8, false) == 3 && b[0] == 0xEF && b[1] == 0xBF && b[2] == 0xBD && utf16_to_utf8(swapped, 2, b, 8, false) == 6 && utf16_to_utf8(high_then_a, 2, b, 8, false) == 4 && b[3] == 0x41,
           "and become U+FFFD otherwise, without eating the character after them");
    expect(utf8_to_utf16(e, 0, w, 16, true) == 0 && utf16_to_utf8(&euro, 0, b, 8, true) == 0, "empty input gives an empty result");
}

using SprintfFn = i32(PEHOST_WINAPI*)(char*, const char*, ...);
SprintfFn ms_sprintf = reinterpret_cast<SprintfFn>(&pehost::msvcrt::sprintf);

struct SinkCollect {
    char text[4096];
    u32 length = 0;
    u32 calls = 0;
    void put(const char* s, u64 n) {
        ++calls;
        for (u64 i = 0; i < n && length + 1 < sizeof(text); ++i) text[length++] = s[i];
        text[length] = 0;
    }
};

PEHOST_WINAPI i64 cvf(SinkCollect& s, const char* f, ...) {
    pehost::fmt::VaList ap;
    __builtin_ms_va_start(ap, f);
    i64 r = pehost::fmt::format(s, f, ap);
    __builtin_ms_va_end(ap);
    return r;
}

PEHOST_WINAPI i32 vs(char* buf, u64 cap, const char* f, ...) {
    pehost::fmt::VaList ap;
    __builtin_ms_va_start(ap, f);
    i32 r = pehost::msvcrt::_vsnprintf(buf, cap, f, ap);
    __builtin_ms_va_end(ap);
    return r;
}

void format_tests() {
    static const char* const strings[] = { "", "a", "hello", "hello world, this is a longer string", "x\ty" };
    static const i64 numbers[] = { 0, 1, -1, 7, -7, 42, 255, 256, -255, 65535, 65536, 2147483647, -2147483647 - 1, 4294967295ll, 4294967296ll, 9223372036854775807ll, -9223372036854775807ll - 1, 123456789012ll };
    char flags_pool[] = "-+ #0";
    u32 bad = 0, bad_ret = 0, tested = 0;
    u32 rounds = g_quick ? 60000 : 600000;
    for (u32 r = 0; r < rounds; ++r) {
        char f[40];
        u32 n = 0;
        u32 lone_l = 0xFFFFFFFFu;
        f[n++] = '%';
        u32 flag_count = static_cast<u32>(below(4));
        char conv = "diuoxXcs"[below(8)];
        bool numeric = conv != 'c' && conv != 's';
        for (u32 i = 0; i < flag_count; ++i) {
            char fl = flags_pool[below(5)];
            if (fl == '0' && !numeric) continue;
            if (fl == '#' && (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'c' || conv == 's')) continue;
            f[n++] = fl;
        }
        bool star_width = below(8) == 0;
        bool star_prec = below(8) == 0;
        i32 width = static_cast<i32>(below(24));
        i32 prec = static_cast<i32>(below(24));
        if (star_width) f[n++] = '*';
        else if (below(2)) n += static_cast<u32>(snprintf(f + n, 8, "%d", width));
        bool has_prec = below(2);
        if (has_prec) {
            f[n++] = '.';
            if (star_prec) f[n++] = '*';
            else if (below(4)) n += static_cast<u32>(snprintf(f + n, 8, "%d", prec));
        }
        if (numeric) {
            u64 lm = below(9);
            if (lm == 1) { f[n++] = 'h'; f[n++] = 'h'; }
            else if (lm == 2) f[n++] = 'h';
            else if (lm == 3) { f[n++] = 'l'; f[n++] = 'l'; }
            else if (lm == 4) f[n++] = 'z';
            else if (lm == 5) { lone_l = n; f[n++] = 'l'; }
            else if (lm == 6) f[n++] = 't';
            else if (lm == 7) f[n++] = 'j';
        }
        f[n++] = conv;
        f[n] = 0;
        char g[40];
        {
            u32 w = 0;
            for (u32 i = 0; f[i]; ++i) {
                if (i == lone_l) continue;
                g[w++] = f[i];
            }
            g[w] = 0;
        }
        bool uses_star_prec = has_prec && star_prec;
        i64 args[3];
        u32 a = 0;
        if (star_width) args[a++] = below(3) == 0 ? -width : width;
        if (uses_star_prec) args[a++] = below(5) == 0 ? -1 : prec;
        const char* sv = strings[below(5)];
        i64 value = conv == 's' ? reinterpret_cast<i64>(sv) : conv == 'c' ? static_cast<i64>(32 + below(90)) : numbers[below(18)];
        args[a++] = value;
        while (a < 3) args[a++] = 0;
        char mine[512], ref[512];
        i32 got = ms_sprintf(mine, f, args[0], args[1], args[2]);
        i32 want = snprintf(ref, sizeof(ref), g, args[0], args[1], args[2]);
        ++tested;
        if (got != want || strcmp(mine, ref) != 0) {
            if (bad < 5) printf("    format %s args %lld %lld %lld -> mine <%s> (%d) glibc <%s> (%d)\n", f, static_cast<long long>(args[0]), static_cast<long long>(args[1]), static_cast<long long>(args[2]), mine, got, ref, want);
            ++bad;
        }
        if (got < 0) ++bad_ret;
    }
    expect(bad == 0 && bad_ret == 0, "600k random integer, string and character formats print exactly what glibc prints, with the same return value");

    char out[600];
    expect(ms_sprintf(out, "%p", reinterpret_cast<void*>(0xABCDEFull)) == 16 && same(out, "0000000000ABCDEF"), "%p prints 16 upper-case hex digits like msvcrt");
    expect(ms_sprintf(out, "%I64d|%I32u|%Id|%hd|%hhu|%lld|%llx", -5ll, 7u, -9ll, 70000, 300, -1ll, 0xFFull) > 0 && same(out, "-5|7|-9|4464|44|-1|ff"), "the Microsoft size prefixes I64, I32 and I, and h and hh, select the width");
    expect(ms_sprintf(out, "%5c|%-5c|%%|%3%|", 'a', 'b') > 0 && contains(out, "    a|b    |%|"), "%c pads and %% prints a percent sign");
    expect(ms_sprintf(out, "%s|%.2s|%10s|%-6.3s|", static_cast<const char*>(nullptr), static_cast<const char*>(nullptr), "abc", "abcdef") > 0 && same(out, "(null)|(n|       abc|abc   |"), "a null string prints (null) and precision cuts it");
    expect(ms_sprintf(out, "%.0d|%.0x|%#.0o|%#o|%#x|%#X|%+d|% d|%+u", 0, 0, 0, 8, 0, 255, 5, 5, 5u) > 0 && same(out, "||0|010|0|0XFF|+5| 5|5"), "zero with zero precision prints nothing, # prefixes, + and space only apply to signed values");
    expect(ms_sprintf(out, "%-8d|%08d|%-08d|%8.3d|%08.3d", 42, -42, 42, 5, 5) > 0 && same(out, "42      |-0000042|42      |     005|     005"), "zero padding goes after the sign, is ignored with - or a precision");
    char big[700];
    for (u32 i = 0; i < 600; ++i) big[i] = static_cast<char>('a' + i % 26);
    big[600] = 0;
    static char wide[3000];
    i32 w = ms_sprintf(wide, "<%s>%1000d|%-700s|", big, 1, "z");
    expect(w == 2304 && wide[0] == '<' && wide[1] == 'a' && wide[600] == big[599] && wide[601] == '>' && wide[602] == ' ' && wide[1601] == '1' && wide[1602] == '|' && wide[1603] == 'z' && wide[1604] == ' ' &&
               wide[2303] == '|' && wide[2304] == 0,
           "output far beyond the 128 byte internal buffer comes through whole and is counted");

    SinkCollect sink;
    i64 r1 = cvf(sink, "ab%q", 1);
    expect(r1 == -1 && same(sink.text, "ab"), "an unsupported conversion ends formatting with -1 after writing what came before");
    SinkCollect fl;
    i64 r2 = cvf(fl, "%f", 1);
    expect(r2 == -1, "floating point conversions are reported as unsupported, not misprinted");
    SinkCollect trailing;
    i64 r3 = cvf(trailing, "100%");
    expect(r3 == -1 || (r3 == 3 && same(trailing.text, "100")), "a lone percent at the end of the format does not read past it");
    SinkCollect plain;
    i64 r4 = cvf(plain, "");
    expect(r4 == 0 && plain.length == 0 && plain.calls == 0, "an empty format writes nothing and does not call the sink");

    char small[8];
    for (u32 i = 0; i < 8; ++i) small[i] = 'Q';
    i32 t1 = vs(small, 4, "%s", "abcdefgh");
    expect(t1 == -1 && small[0] == 'a' && small[3] == 'd' && small[4] == 'Q', "_vsnprintf truncates at the count, returns -1 and writes no terminator");
    for (u32 i = 0; i < 8; ++i) small[i] = 'Q';
    i32 t2 = vs(small, 4, "%s", "abcd");
    expect(t2 == 4 && small[3] == 'd' && small[4] == 'Q', "an exact fit returns the count and again no terminator");
    i32 t3 = vs(small, 8, "%s", "abc");
    expect(t3 == 3 && same(small, "abc"), "a fit with room is terminated");
    i32 t4 = vs(small, 0, "%s", "abc");
    expect(t4 == -1 && small[0] == 'a', "a zero count writes nothing and fails");
}

struct ArgCase {
    const char* line;
    u32 count;
    const char* args[6];
};

void cmdline_tests() {
    static const ArgCase cases[] = {
        { "prog", 1, { "prog" } },
        { "prog a b c", 4, { "prog", "a", "b", "c" } },
        { "  prog   a \t b  ", 3, { "prog", "a", "b" } },
        { "\"C:\\Program Files\\x.exe\" arg", 2, { "C:\\Program Files\\x.exe", "arg" } },
        { "\"prog with space\"", 1, { "prog with space" } },
        { "\"unterminated prog", 1, { "unterminated prog" } },
        { "prog \"abc\" d e", 4, { "prog", "abc", "d", "e" } },
        { "prog a\\\\b d\"e f\"g h", 4, { "prog", "a\\\\b", "de fg", "h" } },
        { "prog a\\\\\\\"b c d", 4, { "prog", "a\\\"b", "c", "d" } },
        { "prog a\\\\\\\\\"b c\" d e", 4, { "prog", "a\\\\b c", "d", "e" } },
        { "prog a\"b\"\" c d", 2, { "prog", "ab\" c d" } },
        { "prog \"\"", 2, { "prog", "" } },
        { "prog a\\\\", 2, { "prog", "a\\\\" } },
        { "prog \"a b", 2, { "prog", "a b" } },
        { "prog \\\\\\\\\"x\"", 2, { "prog", "\\\\x" } },
        { "prog a\tb", 3, { "prog", "a", "b" } },
        { "\"\" a", 2, { "", "a" } },
    };
    u32 good = 0;
    for (const ArgCase& c : cases) {
        char storage[256];
        char* argv[16];
        u32 n = 99;
        bool ok = pehost::cmdline::parse(c.line, storage, sizeof(storage), argv, 16, &n);
        bool match = ok && n == c.count && argv[n] == nullptr;
        for (u32 i = 0; i < c.count && match; ++i) match = same(argv[i], c.args[i]);
        if (match) ++good;
        else {
            printf("    mismatch for <%s>: argc %u", c.line, n);
            for (u32 i = 0; i < n && i < 6; ++i) printf(" <%s>", argv[i]);
            printf("\n");
        }
    }
    expect(good == sizeof(cases) / sizeof(cases[0]), "command lines split by the Microsoft rules: quotes, backslashes before quotes, doubled quotes, tabs, quoted program name");

    char storage[256];
    char* argv[8];
    u32 n = 5;
    expect(pehost::cmdline::parse("", storage, sizeof(storage), argv, 8, &n) && n == 0 && argv[0] == nullptr, "an empty line has no arguments");
    expect(pehost::cmdline::parse("   \t ", storage, sizeof(storage), argv, 8, &n) && n == 0 && argv[0] == nullptr, "a blank line has no arguments");
    expect(pehost::cmdline::parse("a b c d e f g", storage, sizeof(storage), argv, 8, &n) && n == 7 && argv[7] == nullptr, "seven arguments fit eight slots");
    expect(!pehost::cmdline::parse("a b c d e f g h", storage, sizeof(storage), argv, 8, &n) && n == 0, "eight arguments do not, because the last slot is the terminator");
    expect(pehost::cmdline::parse("abcdef", storage, 7, argv, 8, &n) && n == 1 && same(argv[0], "abcdef"), "a program name that exactly fills the storage with its terminator fits");
    expect(!pehost::cmdline::parse("abcdefg", storage, 7, argv, 8, &n) && !pehost::cmdline::parse("a bcdef", storage, 7, argv, 8, &n) && !pehost::cmdline::parse("p \"\\\\\\\\\\\\\\\\\\\\\\\\\"", storage, 7, argv, 8, &n),
           "one byte more does not, in the program name, an argument, or the backslash run before a quote");
    expect(pehost::cmdline::parse("a", storage, 2, argv, 8, &n) && !pehost::cmdline::parse("a", storage, 1, argv, 8, &n) && !pehost::cmdline::parse("a", storage, 0, argv, 8, &n) && !pehost::cmdline::parse("a", storage, 8, argv, 1, &n), "no storage or no argument slots is a failure, not an overrun");
}

struct Recorder {
    u32 exits = 0;
    u32 exit_code = 0;
    u32 writes = 0;
    u32 last_stream = 0;
    char last_text[256];
    u32 last_length = 0;
    u32 protects = 0;
    u8* protect_at = nullptr;
    u64 protect_bytes = 0;
    u32 protect_prot = 0;
    bool protect_ok = true;
    u32 sleeps = 0;
    u32 slept = 0;
} g_rec;

void rec_exit(u32 code) {
    ++g_rec.exits;
    g_rec.exit_code = code;
}

i64 rec_write(u32 stream, const void* data, u32 length) {
    ++g_rec.writes;
    g_rec.last_stream = stream;
    g_rec.last_length = length;
    u32 n = length < 255 ? length : 255;
    for (u32 i = 0; i < n; ++i) g_rec.last_text[i] = static_cast<const char*>(data)[i];
    g_rec.last_text[n] = 0;
    return length;
}

bool rec_protect(u8* at, u64 bytes, u32 prot) {
    ++g_rec.protects;
    g_rec.protect_at = at;
    g_rec.protect_bytes = bytes;
    g_rec.protect_prot = prot;
    return g_rec.protect_ok;
}

void rec_sleep(u32 ms) {
    ++g_rec.sleeps;
    g_rec.slept = ms;
}

pehost::Runtime g_recording = { &rec_exit, &rec_write, &test_map, &rec_protect, &rec_sleep };

void decode_b64(const char* s, u8* out, usize* n) {
    static int table[256];
    static bool ready = false;
    if (!ready) {
        for (int i = 0; i < 256; ++i) table[i] = -1;
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) table[static_cast<u8>(a[i])] = i;
        ready = true;
    }
    u32 acc = 0;
    int bits = 0;
    usize w = 0;
    for (; *s; ++s) {
        int v = table[static_cast<u8>(*s)];
        if (v < 0) continue;
        acc = (acc << 6) | static_cast<u32>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[w++] = static_cast<u8>(acc >> bits);
        }
    }
    *n = w;
}

PEHOST_WINAPI i32 ms_probe_noop() { return 0; }

PEHOST_WINAPI i32 ms_five() { return 5; }

struct ChildResult {
    bool exited;
    i64 code;
    char out[1024];
};

int g_child_out = -1;
int g_child_status = -1;

void child_exit_process(u32 code) {
    i64 status = static_cast<i32>(code);
    lx(SYS_WRITE, static_cast<u64>(g_child_status), reinterpret_cast<u64>(&status), 8);
    lx(SYS_EXIT_GROUP, 0);
    for (;;) asm volatile("ud2");
}

i64 child_console(u32, const void* data, u32 length) { return lx(SYS_WRITE, static_cast<u64>(g_child_out), reinterpret_cast<u64>(data), length); }

pehost::Runtime g_child_runtime = { &child_exit_process, &child_console, &test_map, nullptr, nullptr };

ChildResult in_child(void (*fn)()) {
    ChildResult r{};
    int po[2], ps[2];
    lx(SYS_PIPE, reinterpret_cast<u64>(po));
    lx(SYS_PIPE, reinterpret_cast<u64>(ps));
    i64 pid = lx(SYS_FORK);
    if (pid == 0) {
        g_child_out = po[1];
        g_child_status = ps[1];
        fn();
        i64 returned = 0x7EADBEEF;
        lx(SYS_WRITE, static_cast<u64>(ps[1]), reinterpret_cast<u64>(&returned), 8);
        lx(SYS_EXIT_GROUP, 0);
    }
    lx(SYS_CLOSE, static_cast<u64>(po[1]));
    lx(SYS_CLOSE, static_cast<u64>(ps[1]));
    int wstatus = 0;
    lx(SYS_WAIT4, static_cast<u64>(pid), reinterpret_cast<u64>(&wstatus), 0, 0);
    i64 n = lx(SYS_READ, static_cast<u64>(po[0]), reinterpret_cast<u64>(r.out), sizeof(r.out) - 1);
    r.out[n > 0 ? n : 0] = 0;
    i64 code = 0;
    r.exited = lx(SYS_READ, static_cast<u64>(ps[0]), reinterpret_cast<u64>(&code), 8) == 8 && code != 0x7EADBEEF && (wstatus & 0x7F) == 0;
    r.code = code;
    lx(SYS_CLOSE, static_cast<u64>(po[0]));
    lx(SYS_CLOSE, static_cast<u64>(ps[0]));
    return r;
}

PEHOST_WINAPI void ms_say_one() { pehost::msvcrt::fputs("one;", &pehost::msvcrt::g_iob[1]); }
PEHOST_WINAPI void ms_say_two() { pehost::msvcrt::fputs("two;", &pehost::msvcrt::g_iob[1]); }

void kernel32_tests() {
    using namespace pehost::win32;
    using namespace pehost;
    static pehost::Teb teb;
    static pehost::Peb peb;
    teb.self = reinterpret_cast<u64>(&teb);
    teb.client_thread = 77;
    teb.client_process = 5;
    lx(SYS_ARCH_PRCTL, 0x1001, reinterpret_cast<u64>(&teb));
    pehost::g_runtime = &g_recording;
    g_map_calls = g_map_fail_at = 0;
    g_map_skew = 0;

    static u8 file[0x10000];
    usize file_size = 0;
    decode_b64(pe_sample_mingw_k32, file, &file_size);
    static pe::Image img;
    pe::parse(file, file_size, &img);
    u8* base = map_pages(img.size_of_image + 0x20000);
    base = reinterpret_cast<u8*>((reinterpret_cast<u64>(base) + 0xFFFF) & ~0xFFFFull);
    pe::map(img, file, file_size, base, img.size_of_image);
    pehost::Prepared prepared{};
    prepared.image = img;
    prepared.base = base;
    prepared.peb = &peb;
    pehost::attach(prepared, pehost::all_imports(), pehost::Config{ "C:\\dir\\prog.exe arg \"two words\"", "C:\\dir\\prog.exe" });
    pehost::World& w = pehost::g_world;
    expect(peb.process_heap == reinterpret_cast<u64>(&w.heap) && w.image == base && w.image_pages == img.size_of_image / 4096 && w.heap.chunks() == 0 && w.imports == pehost::all_imports(),
           "attach points the PEB at the process heap and records the image");
    pe::Run runs[pe::MAX_RUNS];
    u32 run_count = pe::plan(img, runs, pe::MAX_RUNS);
    bool plan_ok = run_count > 0;
    for (u32 i = 0; i < run_count; ++i) {
        for (u32 k = 0; k < runs[i].pages; ++k) plan_ok = plan_ok && w.page_prot[runs[i].first_page + k] == runs[i].prot;
    }
    expect(plan_ok && w.page_prot[w.image_pages] == 0, "attach records the planned protection of every image page and nothing beyond");
    expect(same(GetCommandLineA(), "C:\\dir\\prog.exe arg \"two words\"") && GetCommandLineW()[0] == 'C' && GetCommandLineW()[1] == ':' && GetCommandLineW()[31] == 0 && GetCommandLineW() == w.command_line_wide,
           "the command line is available as narrow and wide text");
    u16 wide_expect = 0;
    u32 wl = 0;
    while (GetCommandLineW()[wl]) ++wl;
    expect(wl == pehost::detail::text_length(GetCommandLineA()), "both have the same length");
    (void)wide_expect;

    u64 heap = GetProcessHeap();
    expect(heap == reinterpret_cast<u64>(&w.heap), "GetProcessHeap is the heap in the PEB");
    set_last_error(0);
    u8* hp = static_cast<u8*>(HeapAlloc(heap, 0, 100));
    expect(hp && HeapSize(heap, 0, hp) >= 100 && last_error() == 0, "HeapAlloc and HeapSize");
    for (u32 i = 0; i < 100; ++i) hp[i] = 0xEE;
    HeapFree(heap, 0, hp);
    u8* hz = static_cast<u8*>(HeapAlloc(heap, HEAP_ZERO_MEMORY, 100));
    bool zeroed = true;
    for (u32 i = 0; i < HeapSize(heap, 0, hz); ++i) zeroed = zeroed && hz[i] == 0;
    expect(hz == hp && zeroed, "HEAP_ZERO_MEMORY clears the whole block even when it reuses dirty memory");
    for (u32 i = 0; i < 100; ++i) hz[i] = static_cast<u8>(i + 1);
    u64 before = HeapSize(heap, 0, hz);
    u8* grown = static_cast<u8*>(HeapReAlloc(heap, HEAP_ZERO_MEMORY, hz, 1000));
    bool tail_zero = grown != nullptr;
    for (u64 i = before; tail_zero && i < 1000; ++i) tail_zero = grown[i] == 0;
    bool head_kept = grown != nullptr;
    for (u32 i = 0; head_kept && i < 100; ++i) head_kept = grown[i] == static_cast<u8>(i + 1);
    expect(grown && tail_zero && head_kept, "HeapReAlloc keeps the data and zeroes the added bytes when asked to");
    set_last_error(0);
    expect(HeapReAlloc(heap, HEAP_REALLOC_IN_PLACE_ONLY, grown, 4u << 20) == nullptr && last_error() == pehost::ERROR_NOT_ENOUGH_MEMORY && HeapSize(heap, 0, grown) >= 1000,
           "HEAP_REALLOC_IN_PLACE_ONLY refuses a move and leaves the block alone");
    set_last_error(0);
    expect(HeapReAlloc(heap, 0, grown, 64u << 20) == nullptr && last_error() == pehost::ERROR_NOT_ENOUGH_MEMORY && HeapSize(heap, 0, grown) >= 1000, "an impossible size fails with ERROR_NOT_ENOUGH_MEMORY and keeps the block");
    expect(HeapFree(heap, 0, grown) == 1 && HeapFree(heap, 0, nullptr) == 1, "HeapFree accepts a live block and null");
    set_last_error(0);
    expect(HeapFree(heap, 0, grown) == 0 && last_error() == pehost::ERROR_INVALID_PARAMETER && HeapFree(heap, 0, &teb) == 0, "freeing it twice or freeing a foreign pointer fails with ERROR_INVALID_PARAMETER");
    set_last_error(0);
    expect(HeapAlloc(heap + 16, 0, 8) == nullptr && last_error() == pehost::ERROR_INVALID_HANDLE && HeapFree(0, 0, hz) == 0 && HeapReAlloc(0, 0, hz, 8) == nullptr && HeapSize(0, 0, hz) == ~0ull,
           "a heap handle that is not the process heap is rejected by every call");
    set_last_error(0);
    expect(HeapAlloc(heap, 0, 64u << 20) == nullptr && last_error() == pehost::ERROR_NOT_ENOUGH_MEMORY, "an allocation past the heap cap fails with ERROR_NOT_ENOUGH_MEMORY");
    expect(HeapReAlloc(heap, 0, nullptr, 8) == nullptr && HeapSize(heap, 0, &teb) == ~0ull, "reallocating null or sizing a foreign pointer fails");

    u32 slots[pehost::TLS_SLOTS];
    bool all = true;
    for (u32 i = 0; i < pehost::TLS_SLOTS; ++i) {
        slots[i] = TlsAlloc();
        all = all && slots[i] == i;
    }
    set_last_error(0);
    expect(all && TlsAlloc() == pehost::TLS_OUT_OF_INDEXES && last_error() == pehost::ERROR_NOT_ENOUGH_MEMORY, "TlsAlloc hands out 64 indexes in order and then fails");
    expect(TlsSetValue(5, reinterpret_cast<void*>(0x55)) == 1 && teb.tls_slots[5] == 0x55 && TlsGetValue(5) == reinterpret_cast<void*>(0x55), "slots live in the TEB at offset 0x1480");
    set_last_error(99);
    expect(TlsGetValue(5) == reinterpret_cast<void*>(0x55) && last_error() == 0, "a successful TlsGetValue clears the last error");
    expect(TlsFree(5) == 1 && teb.tls_slots[5] == 0 && TlsAlloc() == 5 && TlsGetValue(5) == nullptr, "a freed index is cleared and handed out again, empty");
    set_last_error(0);
    expect(TlsFree(64) == 0 && last_error() == pehost::ERROR_INVALID_PARAMETER && TlsGetValue(64) == nullptr && TlsSetValue(64, nullptr) == 0 && TlsGetValue(0xFFFFFFFFu) == nullptr, "an index out of range is invalid everywhere");
    TlsFree(7);
    set_last_error(0);
    expect(TlsFree(7) == 0 && last_error() == pehost::ERROR_INVALID_PARAMETER, "freeing an index twice fails");
    for (u32 i = 0; i < pehost::TLS_SLOTS; ++i) TlsFree(i);
    w.tls_used = 0;

    CriticalSection cs;
    InitializeCriticalSection(&cs);
    expect(cs.lock_count == -1 && cs.recursion_count == 0 && cs.owning_thread == 0 && sizeof(cs) == 40, "an initialised critical section is free");
    EnterCriticalSection(&cs);
    EnterCriticalSection(&cs);
    expect(cs.lock_count == 1 && cs.recursion_count == 2 && cs.owning_thread == 77, "entering twice counts recursion and records the owning thread");
    expect(TryEnterCriticalSection(&cs) == 1 && cs.recursion_count == 3, "the owner can enter again with TryEnter");
    cs.owning_thread = 1;
    expect(TryEnterCriticalSection(&cs) == 0 && cs.recursion_count == 3, "TryEnter fails when another thread owns it");
    cs.owning_thread = 77;
    LeaveCriticalSection(&cs);
    LeaveCriticalSection(&cs);
    LeaveCriticalSection(&cs);
    expect(cs.lock_count == -1 && cs.recursion_count == 0 && cs.owning_thread == 0, "leaving as often as entering frees it");
    LeaveCriticalSection(&cs);
    expect(cs.lock_count == -1 && cs.recursion_count == 0, "leaving a free section changes nothing");
    EnterCriticalSection(&cs);
    DeleteCriticalSection(&cs);
    expect(cs.lock_count == -1 && cs.recursion_count == 0 && cs.owning_thread == 0, "deleting resets it");

    u64 self = reinterpret_cast<u64>(base);
    u64 k32 = GetModuleHandleA("kernel32.dll");
    u64 crt = GetModuleHandleA("MSVCRT");
    expect(GetModuleHandleA(nullptr) == self && GetModuleHandleW(nullptr) == self, "GetModuleHandle(NULL) is the image base");
    expect(k32 != 0 && crt != 0 && k32 != crt && k32 != self && GetModuleHandleA("Kernel32") == k32 && GetModuleHandleA("KERNEL32.DLL") == k32 && GetModuleHandleA("msvcrt.dll") == crt,
           "kernel32 and msvcrt have distinct handles, found with or without .dll and in any case");
    static const u16 wk[] = { 'K', 'e', 'r', 'n', 'e', 'l', '3', '2', 0 };
    static const u16 wbad[] = { 'n', 'o', 0 };
    set_last_error(0);
    expect(GetModuleHandleW(wk) == k32 && GetModuleHandleW(wbad) == 0 && last_error() == pehost::ERROR_MOD_NOT_FOUND && GetModuleHandleA("nosuch.dll") == 0, "wide lookups work and unknown modules fail with ERROR_MOD_NOT_FOUND");
    u16 longname[80];
    for (u32 i = 0; i < 79; ++i) longname[i] = 'a';
    longname[79] = 0;
    expect(GetModuleHandleW(longname) == 0 && GetModuleHandleA("kernel32.dllxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx") == 0, "over-long names are not found rather than truncated into a match");
    expect(GetModuleHandleA("kernel3") == 0 && GetModuleHandleA("") == 0 && GetModuleHandleA("kernel32.dl") == 0, "partial names do not match");

    void* getlast = GetProcAddress(k32, "GetLastError");
    expect(getlast == reinterpret_cast<void*>(&GetLastError) && GetProcAddress(crt, "memcpy") == reinterpret_cast<void*>(&pehost::msvcrt::memcpy), "GetProcAddress returns the stub from the right table");
    set_last_error(0);
    expect(GetProcAddress(k32, "memcpy") == nullptr && last_error() == pehost::ERROR_PROC_NOT_FOUND && GetProcAddress(crt, "GetLastError") == nullptr, "a function is only found in its own module");
    set_last_error(0);
    expect(GetProcAddress(self, "main") == nullptr && last_error() == pehost::ERROR_PROC_NOT_FOUND && GetProcAddress(0, "x") == nullptr && GetProcAddress(k32, reinterpret_cast<const char*>(1)) == nullptr, "the image, a null module and an ordinal are not resolved");
    bool every = true;
    for (const pehost::ImportTable* t = pehost::all_imports(); t; t = t->next) {
        for (u32 i = 0; i < t->count; ++i) {
            u64 m = GetModuleHandleA(t->items[i].dll);
            every = every && t->items[i].address != nullptr && GetProcAddress(m, t->items[i].name) == t->items[i].address;
        }
    }
    expect(every, "every import in every table is found by GetProcAddress at its own address");
    u32 total = 0;
    u32 dups = 0;
    for (const pehost::ImportTable* t = pehost::all_imports(); t; t = t->next) {
        for (u32 i = 0; i < t->count; ++i) {
            ++total;
            for (const pehost::ImportTable* u = pehost::all_imports(); u; u = u->next) {
                for (u32 j = 0; j < u->count; ++j) {
                    if (&t->items[i] != &u->items[j] && pehost::detail::same_dll(t->items[i].dll, u->items[j].dll) && pehost::detail::same_name(t->items[i].name, u->items[j].name)) ++dups;
                }
            }
        }
    }
    expect(total > 90 && dups == 0, "the import tables have no duplicate entries");

    char path[16];
    set_last_error(0);
    expect(GetModuleFileNameA(0, path, 16) == 15 && same(path, "C:\\dir\\prog.exe") && GetModuleFileNameA(self, path, 16) == 15, "GetModuleFileName returns the path when it fits exactly with its terminator");
    expect(GetModuleFileNameA(0, path, 15) == 15 && same(path, "C:\\dir\\prog.ex") && last_error() == pehost::ERROR_INSUFFICIENT_BUFFER, "a short buffer gets a terminated truncation and ERROR_INSUFFICIENT_BUFFER");
    set_last_error(0);
    expect(GetModuleFileNameA(0, path, 0) == 0 && last_error() == pehost::ERROR_INSUFFICIENT_BUFFER && GetModuleFileNameA(k32, path, 16) == 0, "a zero size and another module both fail");

    expect(GetCurrentProcess() == ~0ull && GetCurrentThread() == ~1ull && GetCurrentProcessId() == 5 && GetCurrentThreadId() == 77, "pseudo handles and ids come from the TEB");
    g_rec = Recorder{};
    set_last_error(0);
    expect(TerminateProcess(1234, 9) == 0 && last_error() == pehost::ERROR_INVALID_HANDLE && g_rec.exits == 0, "TerminateProcess on another handle fails");
    pehost::g_runtime = &g_child_runtime;
    ChildResult t1 = in_child([] { TerminateProcess(~0ull, 9); });
    ChildResult t2 = in_child([] { ExitProcess(7); });
    pehost::g_runtime = &g_recording;
    expect(t1.exited && t1.code == 9 && t2.exited && t2.code == 7, "TerminateProcess on the current process and ExitProcess end the process with the given code");
    expect(SetUnhandledExceptionFilter(reinterpret_cast<void*>(0x10)) == nullptr && SetUnhandledExceptionFilter(reinterpret_cast<void*>(0x20)) == reinterpret_cast<void*>(0x10) && SetUnhandledExceptionFilter(nullptr) == reinterpret_cast<void*>(0x20),
           "SetUnhandledExceptionFilter returns the previous filter");
    Sleep(25);
    expect(g_rec.sleeps == 1 && g_rec.slept == 25, "Sleep goes to the platform");
    pehost::Runtime no_sleep = g_recording;
    no_sleep.sleep_ms = nullptr;
    pehost::g_runtime = &no_sleep;
    Sleep(5);
    pehost::g_runtime = &g_recording;
    expect(g_rec.sleeps == 1, "Sleep without a platform timer returns at once");

    MemoryBasicInformation mbi;
    u64 text_rva = 0;
    for (u32 i = 0; i < img.section_count; ++i) {
        if (img.sections[i].characteristics & pe::SCN_MEM_EXECUTE) text_rva = img.sections[i].rva;
    }
    u8* text = base + text_rva;
    set_last_error(0);
    expect(VirtualQuery(text + 100, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.base_address == reinterpret_cast<u64>(text) && mbi.allocation_base == self && mbi.protect == 0x20 && mbi.allocation_protect == 0x20 &&
               mbi.state == 0x1000 && mbi.type == 0x1000000 && mbi.region_size >= 4096 && mbi.region_size % 4096 == 0,
           "VirtualQuery on code reports a committed image region, PAGE_EXECUTE_READ, from the page start");
    expect(VirtualQuery(base, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.protect == 2 && mbi.base_address == self, "the headers are PAGE_READONLY");
    u32 data_page = 0;
    for (u32 i = 0; i < w.image_pages; ++i) {
        if (w.page_prot[i] == (pe::PROT_READ | pe::PROT_WRITE)) data_page = i;
    }
    expect(data_page != 0 && VirtualQuery(base + data_page * 4096ull, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.protect == 4, "writable data is PAGE_READWRITE");
    expect(VirtualQuery(base + img.size_of_image, &mbi, sizeof(mbi)) == 0 && last_error() == pehost::ERROR_INVALID_PARAMETER && VirtualQuery(base - 1, &mbi, sizeof(mbi)) == 0 && VirtualQuery(text, &mbi, 47) == 0 && VirtualQuery(text, nullptr, 48) == 0,
           "addresses outside the image, a short structure and a null structure fail with ERROR_INVALID_PARAMETER");
    u32 span = 0;
    while (data_page + span < w.image_pages && w.page_prot[data_page + span] == w.page_prot[data_page]) ++span;
    VirtualQuery(base + data_page * 4096ull, &mbi, sizeof(mbi));
    expect(mbi.region_size == span * 4096ull, "a region ends exactly where the protection changes");

    u32 old = 0;
    g_rec = Recorder{};
    expect(VirtualProtect(text + 10, 20, 4, &old) == 1 && old == 0x20 && g_rec.protects == 1 && g_rec.protect_at == text && g_rec.protect_bytes == 4096 && g_rec.protect_prot == (pe::PROT_READ | pe::PROT_WRITE) &&
               w.page_prot[text_rva / 4096] == (pe::PROT_READ | pe::PROT_WRITE),
           "VirtualProtect widens to whole pages, asks the platform, reports the old protection and records the new one");
    expect(VirtualQuery(text, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.protect == 4, "the change is visible to VirtualQuery");
    expect(VirtualProtect(text, 4096, 0x20, &old) == 1 && old == 4 && g_rec.protect_prot == (pe::PROT_READ | pe::PROT_EXEC), "and can be undone");
    set_last_error(0);
    expect(VirtualProtect(text, 4096, 0x40, &old) == 0 && last_error() == pehost::ERROR_NOT_SUPPORTED && VirtualProtect(text, 4096, 0x80, &old) == 0 && last_error() == pehost::ERROR_NOT_SUPPORTED && g_rec.protects == 2,
           "writable and executable together is refused, nothing is changed");
    set_last_error(0);
    expect(VirtualProtect(text, 4096, 0x1234, &old) == 0 && last_error() == pehost::ERROR_INVALID_PARAMETER && VirtualProtect(text, 0, 4, &old) == 0 && VirtualProtect(base - 4096, 4096, 4, &old) == 0 &&
               VirtualProtect(text, img.size_of_image, 4, &old) == 0 && VirtualProtect(text, 4096, 4, nullptr) == 0 && last_error() == 998 && g_rec.protects == 2,
           "an unknown protection, an empty or outside or overrunning range, and a missing old-protection pointer are refused");
    g_rec.protect_ok = false;
    set_last_error(0);
    expect(VirtualProtect(text, 4096, 4, &old) == 0 && last_error() == pehost::ERROR_NOT_ENOUGH_MEMORY && w.page_prot[text_rva / 4096] == (pe::PROT_READ | pe::PROT_EXEC), "when the platform refuses, nothing is recorded as changed");
    g_rec.protect_ok = true;
    expect(VirtualProtect(text, 4096, 1, &old) == 1 && w.page_prot[text_rva / 4096] == 0 && VirtualQuery(text, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.protect == 1, "PAGE_NOACCESS maps to no rights");
    expect(VirtualProtect(text, 4096, 2, &old) == 1 && old == 1 && w.page_prot[text_rva / 4096] == pe::PROT_READ, "PAGE_READONLY maps to read");
    expect(VirtualProtect(text, 4096, 8, &old) == 1 && w.page_prot[text_rva / 4096] == (pe::PROT_READ | pe::PROT_WRITE), "PAGE_WRITECOPY maps to read and write");
    expect(VirtualProtect(text, 4096, 0x10, &old) == 1 && w.page_prot[text_rva / 4096] == (pe::PROT_READ | pe::PROT_EXEC), "PAGE_EXECUTE maps to read and execute");
    VirtualProtect(text, 4096, 0x20, &old);

    char mb[64];
    u16 wb[32];
    set_last_error(0);
    expect(MultiByteToWideChar(65001, 0, "h\xC3\xA9llo", -1, wb, 32) == 6 && wb[1] == 0xE9 && wb[5] == 0, "MultiByteToWideChar converts UTF-8 and counts the terminator for length -1");
    expect(MultiByteToWideChar(0, 0, "abc", 3, wb, 32) == 3 && MultiByteToWideChar(0, 0, "abc", -1, nullptr, 0) == 4 && MultiByteToWideChar(1, 0, "abc", 2, nullptr, 0) == 2 && MultiByteToWideChar(3, 0, "abc", 1, wb, 1) == 1,
           "the ANSI, OEM and thread code pages are UTF-8 here, and a null buffer asks for the size");
    set_last_error(0);
    expect(MultiByteToWideChar(65001, 0, "abc", -1, wb, 2) == 0 && last_error() == pehost::ERROR_INSUFFICIENT_BUFFER, "a short output buffer fails with ERROR_INSUFFICIENT_BUFFER");
    set_last_error(0);
    expect(MultiByteToWideChar(65001, MB_ERR_INVALID_CHARS, "a\xFF", 2, wb, 8) == 0 && last_error() == pehost::ERROR_NO_UNICODE_TRANSLATION && MultiByteToWideChar(65001, 0, "a\xFF", 2, wb, 8) == 2 && wb[1] == 0xFFFD,
           "invalid UTF-8 fails with MB_ERR_INVALID_CHARS and is replaced without it");
    set_last_error(0);
    expect(MultiByteToWideChar(1252, 0, "a", 1, wb, 8) == 0 && last_error() == pehost::ERROR_INVALID_PARAMETER && MultiByteToWideChar(65001, 0x100, "a", 1, wb, 8) == 0 && last_error() == pehost::ERROR_INVALID_FLAGS &&
               MultiByteToWideChar(65001, 0, nullptr, 1, wb, 8) == 0 && MultiByteToWideChar(65001, 0, "a", 0, wb, 8) == 0 && MultiByteToWideChar(65001, 0, "a", -2, wb, 8) == 0 && MultiByteToWideChar(65001, 0, "a", 1, nullptr, 4) == 0 &&
               MultiByteToWideChar(65001, 0, "a", 1, wb, -1) == 0,
           "an unsupported code page, unknown flags, a null source, zero or bad length, and a bad buffer are all refused");
    u16 smile[3] = { 0xD83D, 0xDE00, 0 };
    expect(WideCharToMultiByte(65001, 0, smile, -1, mb, 64, nullptr, nullptr) == 5 && static_cast<u8>(mb[0]) == 0xF0 && mb[4] == 0 && WideCharToMultiByte(65001, 0, smile, 2, nullptr, 0, nullptr, nullptr) == 4,
           "WideCharToMultiByte encodes a surrogate pair as four bytes and sizes with a null buffer");
    set_last_error(0);
    expect(WideCharToMultiByte(65001, 0, smile, -1, mb, 4, nullptr, nullptr) == 0 && last_error() == pehost::ERROR_INSUFFICIENT_BUFFER, "a short output buffer is reported");
    u16 lone[2] = { 0xD800, 0 };
    set_last_error(0);
    expect(WideCharToMultiByte(65001, WC_ERR_INVALID_CHARS, lone, 1, mb, 8, nullptr, nullptr) == 0 && last_error() == pehost::ERROR_NO_UNICODE_TRANSLATION && WideCharToMultiByte(65001, 0, lone, 1, mb, 8, nullptr, nullptr) == 3,
           "a lone surrogate fails with WC_ERR_INVALID_CHARS and becomes U+FFFD without it");
    i32 used = 5;
    set_last_error(0);
    expect(WideCharToMultiByte(65001, 0, smile, 2, mb, 8, "?", nullptr) == 0 && WideCharToMultiByte(65001, 0, smile, 2, mb, 8, nullptr, &used) == 0 && WideCharToMultiByte(0, 0, smile, 2, mb, 8, "?", &used) == 4 && used == 0,
           "a default character is an error for UTF-8, accepted for the other code pages, and nothing is ever substituted");
    set_last_error(0);
    expect(WideCharToMultiByte(1252, 0, smile, 2, mb, 8, nullptr, nullptr) == 0 && WideCharToMultiByte(65001, 0, nullptr, 2, mb, 8, nullptr, nullptr) == 0 && WideCharToMultiByte(65001, 0, smile, 0, mb, 8, nullptr, nullptr) == 0 &&
               WideCharToMultiByte(65001, 0, smile, 2, nullptr, 4, nullptr, nullptr) == 0 && WideCharToMultiByte(65001, 0, smile, -2, mb, 8, nullptr, nullptr) == 0,
           "an unsupported code page, a null source, zero or bad length and a bad buffer are refused");
    expect(IsDBCSLeadByteEx(0, 0x81) == 0 && IsDBCSLeadByteEx(932, 0xE0) == 0 && GetACP() == 65001, "there are no double byte lead bytes and the ANSI code page is UTF-8");

    expect(GetStdHandle(pehost::STD_OUTPUT) == pehost::HANDLE_STDOUT, "the standard handles are unchanged");
}


u64 g_rec_sizes[16];
u32 g_rec_count = 0;
u8* recording_map(u64 bytes) {
    if (g_rec_count < 16) g_rec_sizes[g_rec_count] = bytes;
    ++g_rec_count;
    return test_map(bytes);
}

void heap_edge_tests() {
    static pehost::Heap h;
    g_map_calls = g_map_fail_at = 0;
    g_map_skew = 0;
    h.init(&test_map);
    u8* fit = static_cast<u8*>(h.alloc(262112));
    expect(fit && h.chunks() == 1 && h.mapped() == 262144 && h.usable(fit) == 262112 && h.check(), "a request that exactly fills the first chunk does not map a second one");
    u64* hdr = reinterpret_cast<u64*>(fit - 16);
    u64 saved = *hdr;
    *hdr = (262144ull) | 3;
    expect(!h.free(fit) && !h.check(), "a block claiming to extend past its chunk is refused and caught by the checker");
    u64* beyond = reinterpret_cast<u64*>(fit - 16 + 262144);
    u64 beyond_saved = *beyond;
    *hdr = (262144ull) | 3;
    *beyond = 2;
    expect(!h.free(fit) && !h.owns(fit), "even when the memory after the claimed end looks like a live block");
    *beyond = beyond_saved;
    *hdr = saved;
    expect(h.free(fit) && h.check(), "and the real block frees normally");
    h.init(&test_map);
    u8* over = static_cast<u8*>(h.alloc(262113));
    expect(over && h.chunks() == 1 && h.mapped() == 266240 && h.check(), "one byte more maps a chunk rounded up to whole pages");

    h.init(&test_map);
    u8* a = static_cast<u8*>(h.alloc(48));
    u8* guard = static_cast<u8*>(h.alloc(1));
    h.free(a);
    u8* again = static_cast<u8*>(h.alloc(48));
    expect(again == a && h.usable(again) == 48 && h.check(), "a free block of exactly the right size is reused whole");
    h.free(again);
    u8* low = static_cast<u8*>(h.alloc(16));
    u8* high = static_cast<u8*>(h.alloc(16));
    expect(low == a && high == a + 32 && h.usable(low) == 16 && h.usable(high) == 16 && h.check(), "a remainder of exactly the minimum block size is split off");
    h.free(low);
    h.free(high);
    u8* mid = static_cast<u8*>(h.alloc(32));
    h.free(mid);
    u8* fill = static_cast<u8*>(h.alloc(16));
    expect(fill == a && h.usable(fill) == 16 && h.check(), "a split leaves the front block at the requested size");
    h.free(fill);
    h.free(guard);

    h.init(&test_map);
    u8* s1 = static_cast<u8*>(h.alloc(32));
    u8* s2 = static_cast<u8*>(h.alloc(1));
    expect(h.usable(s1) == 32, "a 48 byte block holds 32 bytes");
    expect(h.resize(s1, 16) == s1 && h.usable(s1) == 32 && h.check(), "shrinking by less than the minimum block keeps the block whole");
    u8* s3 = static_cast<u8*>(h.alloc(48));
    expect(h.resize(s3, 16) == s3 && h.usable(s3) == 16 && h.check(), "shrinking by exactly the minimum block splits it");
    u8* t = static_cast<u8*>(h.alloc(16));
    expect(t == s3 + 32 && h.check(), "the split-off tail is allocatable");
    h.free(t);
    h.free(s1);
    h.free(s2);
    h.free(s3);

    h.init(&test_map);
    u8* f = static_cast<u8*>(h.alloc(16));
    u8* g = static_cast<u8*>(h.alloc(16));
    u64* fh = reinterpret_cast<u64*>(f - 16);
    u64 fs = *fh;
    *fh = 16 | 3;
    expect(!h.free(f) && !h.owns(f) && h.usable(f) == 0, "a live looking block smaller than the minimum is not accepted");
    u64* fp = reinterpret_cast<u64*>(f);
    u64 fp_saved = *fp;
    *fp = 2;
    expect(!h.free(f) && !h.owns(f), "even when the bytes after it look like a following live block");
    *fp = fp_saved;
    *fh = fs;
    expect(h.free(f) && h.free(g) && h.check(), "and the real ones free");

    h.init(&test_map);
    u8* ca = static_cast<u8*>(h.alloc(16));
    u8* cb = static_cast<u8*>(h.alloc(16));
    u8* cc = static_cast<u8*>(h.alloc(16));
    u8* cd = static_cast<u8*>(h.alloc(16));
    h.free(ca);
    h.free(cc);
    u64* cbh = reinterpret_cast<u64*>(cb - 16);
    u64 cb_saved = *cbh;
    *cbh &= ~1ull;
    expect(!h.check(), "two adjacent free blocks are reported by the checker");
    *cbh = cb_saved;
    expect(h.check() && h.free(cb) && h.free(cd) && h.check(), "and the consistent heap passes and coalesces");

    g_rec_count = 0;
    h.init(&recording_map);
    u32 n = 0;
    while (h.alloc(250000)) ++n;
    static const u64 want[] = { 262144, 524288, 1048576, 2097152, 2097152, 2097152, 253952 };
    bool sizes = g_rec_count == 7;
    for (u32 i = 0; i < 7 && i < g_rec_count; ++i) sizes = sizes && g_rec_sizes[i] == want[i];
    expect(sizes && h.chunks() == 7 && h.mapped() == 8380416 && h.check(), "chunks double from 256 KiB up to 2 MiB, and the last is just big enough once the cap is near");
    expect(n == 32, "thirty-two 250000 byte blocks fit in the chunks, and no more");
    h.init(&test_map);
    u8* edge = static_cast<u8*>(h.alloc(pehost::Heap::CAP - 4096 - 32));
    expect(edge && h.mapped() == 8384512 && h.mapped() <= pehost::Heap::CAP && h.check() && h.alloc(8192) == nullptr, "a request just under the cap maps exactly what it needs and leaves no room for more");
}

void cmdline_edge_tests() {
    static const char* const lines[] = {
        "prog", "prog a", "prog a b c", "prog \"a b\" c", "prog a\\\\\"b c d", "prog \\\\\\\"x\\\\\\\" y",
        "\"my prog\" \"\" x", "p \"a\"\"b\" q", "p a\\\\\\\\\"b c\" d", "  prog   a    b  ", "p a\\\\b c\\d", "p \"unterminated arg", "\"quoted prog\" \\\\\\\\ x",
    };
    u32 bad = 0;
    for (const char* line : lines) {
        char big[512];
        char* av[32];
        u32 count = 0;
        if (!pehost::cmdline::parse(line, big, sizeof(big), av, 32, &count)) {
            ++bad;
            continue;
        }
        u32 need = 0;
        for (u32 i = 0; i < count; ++i) {
            u32 l = 0;
            while (av[i][l]) ++l;
            need += l + 1;
        }
        bool all = true;
        for (u32 cap = 0; cap <= need + 2; ++cap) {
            char* tight = static_cast<char*>(malloc(cap ? cap : 1));
            char* av2[32];
            u32 count2 = 0;
            bool ok = pehost::cmdline::parse(line, tight, cap, av2, 32, &count2);
            if (cap < need) {
                all = all && !ok;
            } else {
                all = all && ok && count2 == count;
                for (u32 i = 0; i < count && ok; ++i) all = all && same(av[i], av2[i]);
            }
            free(tight);
        }
        char* av3[32];
        u32 count3 = 0;
        bool slots_fit = pehost::cmdline::parse(line, big, sizeof(big), av3, count + 1, &count3) && count3 == count;
        bool slots_fail = !pehost::cmdline::parse(line, big, sizeof(big), av3, count, &count3);
        if (!all || !slots_fit || !slots_fail) {
            printf("    capacity sweep failed for <%s>: need %u all %d slots %d %d\n", line, need, all, slots_fit, slots_fail);
            ++bad;
        }
    }
    expect(bad == 0, "for every sample line every capacity below the need fails and every capacity from it up gives the same arguments, in exactly sized memory");
}

void kernel32_edge_tests() {
    using namespace pehost::win32;
    using namespace pehost;
    static pehost::Teb teb;
    static pehost::Peb peb;
    teb.self = reinterpret_cast<u64>(&teb);
    teb.client_thread = 77;
    teb.client_process = 5;
    lx(SYS_ARCH_PRCTL, 0x1001, reinterpret_cast<u64>(&teb));
    pehost::g_runtime = &g_recording;
    g_map_calls = g_map_fail_at = 0;
    g_map_skew = 0;

    static u8 file[0x10000];
    usize file_size = 0;
    decode_b64(pe_sample_mingw_k32, file, &file_size);
    static pe::Image img;
    pe::parse(file, file_size, &img);
    u8* base = map_pages(img.size_of_image + 0x20000);
    base = reinterpret_cast<u8*>((reinterpret_cast<u64>(base) + 0xFFFF) & ~0xFFFFull);
    pe::map(img, file, file_size, base, img.size_of_image);
    pehost::Prepared prepared{};
    prepared.image = img;
    prepared.base = base;
    prepared.peb = &peb;

    static char longline[2100];
    for (u32 i = 0; i < 2099; ++i) longline[i] = i % 7 == 6 ? ' ' : 'a';
    longline[2099] = 0;
    static char longpath[400];
    for (u32 i = 0; i < 399; ++i) longpath[i] = 'p';
    longpath[399] = 0;
    pehost::World& w = pehost::g_world;
    for (u32 i = 0; i < pehost::MAX_IMAGE_PAGES; ++i) w.page_prot[i] = 0xAA;
    pehost::attach(prepared, pehost::all_imports(), pehost::Config{ longline, longpath });
    u32 cl = 0;
    while (w.command_line[cl]) ++cl;
    u32 mp = 0;
    while (w.module_path[mp]) ++mp;
    u32 wl = 0;
    while (w.command_line_wide[wl]) ++wl;
    expect(cl == pehost::MAX_COMMAND_LINE - 1 && mp == pehost::MAX_MODULE_PATH - 1 && wl == cl && w.command_line[0] == 'a' && w.command_line[cl - 1] == longline[cl - 1],
           "an over-long command line and module path are truncated to fit with a terminator, and the wide copy matches");
    bool beyond_clear = w.page_prot[w.image_pages] == 0xAA || w.page_prot[w.image_pages] == 0;
    bool inside_set = true;
    for (u32 i = 0; i < w.image_pages; ++i) inside_set = inside_set && w.page_prot[i] != 0xAA;
    bool all_clear = true;
    for (u32 i = w.image_pages; i < pehost::MAX_IMAGE_PAGES; ++i) all_clear = all_clear && w.page_prot[i] == 0;
    expect(inside_set && all_clear && beyond_clear, "attach overwrites stale protection for every page, inside and beyond the image");

    pehost::attach(prepared, pehost::all_imports(), pehost::Config{ "C:\\dir\\prog.exe", "C:\\dir\\prog.exe" });
    u64 heap = GetProcessHeap();
    expect(same(w.module_path, "C:\\dir\\prog.exe") && same(w.command_line, "C:\\dir\\prog.exe"), "attaching again replaces the previous text");

    u8* dirty = static_cast<u8*>(HeapAlloc(heap, 0, 64));
    for (u32 i = 0; i < 64; ++i) dirty[i] = 0xCC;
    HeapFree(heap, 0, dirty);
    u8* reuse = static_cast<u8*>(HeapAlloc(heap, 0, 64));
    bool kept = reuse == dirty;
    u32 dirty_count = 0;
    for (u32 i = 16; i < 64; ++i) dirty_count += reuse[i] == 0xCC;
    expect(kept && dirty_count > 0, "without HEAP_ZERO_MEMORY a reused block is not cleared");
    HeapFree(heap, 0, reuse);

    u8* ra = static_cast<u8*>(HeapAlloc(heap, 0, 32));
    u8* rc = static_cast<u8*>(HeapAlloc(heap, 0, 200));
    for (u32 i = 0; i < 200; ++i) rc[i] = 0xCC;
    HeapFree(heap, 0, rc);
    u8* grown_plain = static_cast<u8*>(HeapReAlloc(heap, 0, ra, 150));
    expect(grown_plain == ra && grown_plain[120] == 0xCC, "without HEAP_ZERO_MEMORY a grown block keeps whatever the new bytes held");
    u8* grown_zero = static_cast<u8*>(HeapReAlloc(heap, HEAP_ZERO_MEMORY, grown_plain, 190));
    expect(grown_zero && grown_zero[185] == 0, "and with it the new bytes are cleared");
    HeapFree(heap, 0, grown_zero);

    set_last_error(0);
    expect(HeapReAlloc(heap, 0, &teb, 8) == nullptr && last_error() == pehost::ERROR_INVALID_PARAMETER, "HeapReAlloc of a foreign pointer is ERROR_INVALID_PARAMETER");
    u8* blk = static_cast<u8*>(HeapAlloc(heap, 0, 100));
    u64 usable_now = HeapSize(heap, 0, blk);
    set_last_error(0);
    u8* same_size = static_cast<u8*>(HeapReAlloc(heap, HEAP_REALLOC_IN_PLACE_ONLY, blk, usable_now));
    expect(same_size == blk && last_error() == 0, "HEAP_REALLOC_IN_PLACE_ONLY succeeds when the size equals the usable size");
    set_last_error(0);
    expect(HeapReAlloc(heap, HEAP_REALLOC_IN_PLACE_ONLY, blk, usable_now + 1) == nullptr && last_error() == pehost::ERROR_NOT_ENOUGH_MEMORY, "and fails one byte over");
    set_last_error(0);
    expect(HeapSize(heap, 0, &teb) == ~0ull && last_error() == pehost::ERROR_INVALID_PARAMETER && HeapSize(heap + 1, 0, blk) == ~0ull && last_error() == pehost::ERROR_INVALID_HANDLE,
           "HeapSize reports a bad pointer and a bad heap with different errors");
    HeapFree(heap, 0, blk);

    CriticalSection cs;
    InitializeCriticalSection(&cs);
    EnterCriticalSection(&cs);
    EnterCriticalSection(&cs);
    LeaveCriticalSection(&cs);
    expect(cs.owning_thread == 77 && cs.recursion_count == 1 && cs.lock_count == 0, "the owner is kept until the last leave");
    LeaveCriticalSection(&cs);

    set_last_error(0);
    expect(GetModuleHandleA("kernel32.dll") != 0 && last_error() == 0, "a successful lookup leaves the last error alone");
    set_last_error(0);
    GetModuleHandleA("nosuch");
    expect(last_error() == pehost::ERROR_MOD_NOT_FOUND, "a failed narrow lookup sets ERROR_MOD_NOT_FOUND");
    u16 w63[64], w64[65];
    for (u32 i = 0; i < 63; ++i) w63[i] = 'a';
    w63[63] = 0;
    for (u32 i = 0; i < 64; ++i) w64[i] = 'a';
    w64[64] = 0;
    set_last_error(0);
    expect(GetModuleHandleW(w63) == 0 && last_error() == pehost::ERROR_MOD_NOT_FOUND, "a 63 character wide name is looked up and not found");
    set_last_error(0);
    expect(GetModuleHandleW(w64) == 0 && last_error() == pehost::ERROR_MOD_NOT_FOUND, "a 64 character wide name is rejected");
    char n34[40], n35[40];
    const char* tail = "kernel32.dll";
    u32 k = 0;
    for (; k < 22; ++k) n34[k] = 'x';
    for (u32 i = 0; i < 12; ++i) n34[k++] = tail[i];
    n34[k] = 0;
    u32 j = 0;
    for (; j < 23; ++j) n35[j] = 'x';
    for (u32 i = 0; i < 12; ++i) n35[j++] = tail[i];
    n35[j] = 0;
    expect(GetModuleHandleA(n34) == 0 && GetModuleHandleA(n35) == 0, "long unknown names near the buffer limit fail cleanly");
    char dotless[36];
    u32 d = 0;
    for (; d < 31; ++d) dotless[d] = 'k';
    dotless[d] = 0;
    expect(GetModuleHandleA(dotless) == 0, "a long name without a dot gets the extension appended safely and is not found");

    char* guarded = reinterpret_cast<char*>(map_pages(8192));
    guarded[4096] = 'Q';
    char* exact = guarded + 4096 - 16;
    for (u32 i = 0; i < 16; ++i) exact[i] = 'Z';
    set_last_error(0);
    expect(GetModuleFileNameA(0, exact, 16) == 15 && same(exact, "C:\\dir\\prog.exe") && guarded[4096] == 'Q', "GetModuleFileName fills an exactly sized buffer without writing past it");
    for (u32 i = 0; i < 16; ++i) exact[i] = 'Z';
    expect(GetModuleFileNameA(0, exact, 15) == 15 && exact[14] == 0 && exact[13] == 'x' && guarded[4096] == 'Q' && last_error() == pehost::ERROR_INSUFFICIENT_BUFFER, "and a one byte shorter buffer is filled and terminated");
    expect(GetModuleFileNameA(0, exact, 1) == 1 && exact[0] == 0, "a one byte buffer receives only the terminator");

    MemoryBasicInformation mbi;
    pe::Run runs[pe::MAX_RUNS];
    u32 run_count = pe::plan(img, runs, pe::MAX_RUNS);
    bool regions = run_count > 0;
    for (u32 i = 0; i < run_count; ++i) {
        for (u32 k = 0; k < runs[i].pages; ++k) {
            u32 page = runs[i].first_page + k;
            u32 end = page;
            while (end < w.image_pages && w.page_prot[end] == w.page_prot[page]) ++end;
            u8* at = base + page * 4096ull;
            regions = regions && VirtualQuery(at + 4095, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.base_address == reinterpret_cast<u64>(at) && mbi.region_size == (end - page) * 4096ull;
        }
    }
    expect(regions, "VirtualQuery reports the true region for every planned run, from the last byte of each of its pages: the base is the page and the size runs to where the protection changes");
    expect(VirtualQuery(base + (w.image_pages - 1) * 4096ull, &mbi, sizeof(mbi)) == sizeof(mbi) && VirtualQuery(base + w.image_pages * 4096ull - 1, &mbi, sizeof(mbi)) == sizeof(mbi) && VirtualQuery(base + w.image_pages * 4096ull, &mbi, sizeof(mbi)) == 0,
           "the last image byte is queryable and the next is not");

    u64 text_rva = 0;
    for (u32 i = 0; i < img.section_count; ++i) {
        if (img.sections[i].characteristics & pe::SCN_MEM_EXECUTE) text_rva = img.sections[i].rva;
    }
    u8* text = base + text_rva;
    u32 old = 0;
    g_rec = Recorder{};
    expect(VirtualProtect(text, 4096, 4, &old) == 1 && g_rec.protect_bytes == 4096 && g_rec.protect_at == text, "an aligned one page protect asks for exactly one page");
    VirtualProtect(text, 4096, 0x20, &old);
    g_rec = Recorder{};
    expect(VirtualProtect(text + 4095, 2, 4, &old) == 1 && g_rec.protect_bytes == 8192 && g_rec.protect_at == text, "a range straddling a page boundary covers both pages");
    u32 first_page = static_cast<u32>(text_rva / 4096);
    expect(w.page_prot[first_page] == (pe::PROT_READ | pe::PROT_WRITE) && w.page_prot[first_page + 1] == (pe::PROT_READ | pe::PROT_WRITE) && w.page_prot[first_page - 1] != (pe::PROT_READ | pe::PROT_WRITE),
           "exactly the pages in the range are recorded as changed");
    VirtualProtect(text, 8192, 0x20, &old);
    g_rec = Recorder{};
    u8* at_base = base;
    expect(VirtualProtect(at_base, 1, 2, &old) == 1 && g_rec.protect_at == base && g_rec.protect_bytes == 4096, "protecting the first byte of the image works");
    u64 whole = static_cast<u64>(w.image_pages) * 4096;
    g_rec = Recorder{};
    expect(VirtualProtect(base + whole - 1, 1, 2, &old) == 1 && g_rec.protect_at == base + whole - 4096 && VirtualProtect(base + whole - 1, 2, 2, &old) == 0, "the last byte of the image can be protected and one more cannot");
    expect(VirtualProtect(base, whole, 2, &old) == 1 && g_rec.protect_bytes == whole && VirtualProtect(base, whole + 1, 2, &old) == 0, "the whole image can be protected and not one byte more");
    set_last_error(0);
    expect(VirtualProtect(base + whole, 1, 2, &old) == 0 && last_error() == pehost::ERROR_INVALID_PARAMETER && VirtualProtect(base - 1, 1, 2, &old) == 0, "just past either end is invalid");
    g_rec = Recorder{};
    expect(VirtualProtect(base + whole + 4096, 1, 2, &old) == 0 && VirtualProtect(base + 3 * whole, 8, 2, &old) == 0 && g_rec.protects == 0, "well past the end is invalid and nothing is asked of the platform");
    pehost::attach(prepared, pehost::all_imports(), pehost::Config{ "C:\\dir\\prog.exe", "C:\\dir\\prog.exe" });

    {
        pehost::Prepared more = prepared;
        more.image.size_of_image += 8192;
        pehost::attach(more, pehost::all_imports(), pehost::Config{ "p", "p" });
        u32 n = w.image_pages;
        MemoryBasicInformation q;
        u32 old_prot = 0;
        bool readable_tail = w.page_prot[n - 1] == pe::PROT_READ && w.page_prot[n - 2] == pe::PROT_READ && w.page_prot[n] == 0;
        bool two_pages = VirtualQuery(base + (n - 2) * 4096ull, &q, sizeof(q)) == sizeof(q) && q.protect == 2 && q.region_size == 8192;
        bool revoked = VirtualProtect(base + (n - 1) * 4096ull, 1, 1, &old_prot) == 1;
        bool one_page = VirtualQuery(base + (n - 1) * 4096ull, &q, sizeof(q)) == sizeof(q) && q.protect == 1 && q.region_size == 4096;
        bool before = VirtualQuery(base + (n - 2) * 4096ull, &q, sizeof(q)) == sizeof(q) && q.protect == 2 && q.region_size == 4096;
        expect(readable_tail && two_pages && revoked && one_page && before, "pages after the last section are read-only, and a region of no-access pages stops at the end of the image");
        pehost::Prepared fewer = prepared;
        fewer.image.size_of_image -= 4096;
        pehost::attach(fewer, pehost::all_imports(), pehost::Config{ "p", "p" });
        u32 m = w.image_pages;
        bool clipped = true;
        for (u32 i = m; i < pehost::MAX_IMAGE_PAGES; ++i) clipped = clipped && w.page_prot[i] == 0;
        expect(m == n - 3, "an image shrunk by a page has one page fewer");
        expect(clipped, "protections are never recorded beyond the end of the image");
    }
    pehost::attach(prepared, pehost::all_imports(), pehost::Config{ "C:\\dir\\prog.exe arg \"two words\"", "C:\\dir\\prog.exe" });
    u16 wb[8];
    set_last_error(0);
    expect(MultiByteToWideChar(65001, 0, "a", 1, wb, 8) == 1 && last_error() == 0, "a success leaves the last error alone");
    char mb[16];
    set_last_error(0);
    u16 one[1] = { 'a' };
    expect(WideCharToMultiByte(65001, 0, one, 1, mb, 16, nullptr, nullptr) == 1 && last_error() == 0, "a wide success leaves the last error alone");
    set_last_error(0);
    MultiByteToWideChar(65001, 0, "a", 0, wb, 8);
    expect(last_error() == pehost::ERROR_INVALID_PARAMETER, "a zero length is ERROR_INVALID_PARAMETER");
    set_last_error(0);
    MultiByteToWideChar(65001, 0, "a", -2, wb, 8);
    expect(last_error() == pehost::ERROR_INVALID_PARAMETER, "a length below -1 is ERROR_INVALID_PARAMETER");
    set_last_error(0);
    WideCharToMultiByte(65001, 0, one, 0, mb, 16, nullptr, nullptr);
    expect(last_error() == pehost::ERROR_INVALID_PARAMETER, "a wide zero length is ERROR_INVALID_PARAMETER");
    set_last_error(0);
    WideCharToMultiByte(65001, 0, one, -2, mb, 16, nullptr, nullptr);
    expect(last_error() == pehost::ERROR_INVALID_PARAMETER, "a wide length below -1 is ERROR_INVALID_PARAMETER");
}

i32 g_trap = 0;
PEHOST_WINAPI i32 trap_fn() {
    ++g_trap;
    return 0;
}

void msvcrt_edge_tests() {
    namespace crt = pehost::msvcrt;
    using pehost::msvcrt::g_iob;
    crt::reset();
    pehost::g_runtime = &g_recording;
    g_rec = Recorder{};
    *crt::_errno() = 0;
    char out[32];
    expect(crt::_snprintf(out, 32, "") == 0 && *crt::_errno() == 0 && out[0] == 0, "an empty format prints nothing and leaves errno alone");
    expect(crt::printf("") == 0 && *crt::_errno() == 0, "printf of an empty format returns zero");
    expect(crt::calloc(0x8000000000000001ull, 2) == nullptr && *crt::_errno() == crt::ERR_NOMEM, "calloc with an overflowing product fails with ENOMEM");
    void* c = crt::calloc(1, 64);
    bool zero = c != nullptr;
    for (u32 i = 0; zero && i < 64; ++i) zero = static_cast<u8*>(c)[i] == 0;
    expect(zero && pehost::g_world.heap.check(), "calloc clears what it returns and leaves the heap consistent");
    crt::free(c);
    u8 src[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    u8 dst[10] = { 9, 9, 9, 9, 9, 9, 9, 9, 9, 9 };
    crt::memcpy(dst, src, 8);
    expect(dst[0] == 1 && dst[7] == 8 && dst[8] == 9 && dst[9] == 9, "memcpy copies exactly the requested bytes");
    g_rec = Recorder{};
    expect(crt::printf("%1000000005d", 1) == 100000000 && crt::printf("%.1000000005d", 1) == 100000000, "a width or precision past a hundred million stops growing at it");
    g_trap = 0;
    void* table[3] = { nullptr, nullptr, reinterpret_cast<void*>(&trap_fn) };
    crt::_initterm_e(table, table + 2);
    crt::_initterm(table, table + 2);
    expect(g_trap == 0, "_initterm and _initterm_e stop before the end of the range");
    void* live[3] = { reinterpret_cast<void*>(&trap_fn), nullptr, reinterpret_cast<void*>(&trap_fn) };
    crt::_initterm_e(live, live + 3);
    expect(g_trap == 2, "and run every entry before it, skipping empty ones");
    for (u32 i = 0; i < crt::MAX_ONEXIT; ++i) crt::g_onexit[i] = reinterpret_cast<void*>(0x10 + i);
    crt::g_onexit_count = 3;
    crt::reset();
    bool onexit_clear = crt::g_onexit_count == 0;
    for (u32 i = 0; i < crt::MAX_ONEXIT; ++i) onexit_clear = onexit_clear && crt::g_onexit[i] == nullptr;
    expect(onexit_clear, "reset clears the whole exit-handler table");
    crt::signal(2, reinterpret_cast<void*>(0x1234));
    crt::signal(15, reinterpret_cast<void*>(0x5678));
    crt::reset();
    expect(crt::signal(2, nullptr) == nullptr && crt::signal(15, nullptr) == nullptr, "reset clears every installed signal handler");
}

PEHOST_WINAPI i32 order_a() {
    g_rec.writes += 1;
    return 0;
}

void msvcrt_tests() {
    namespace crt = pehost::msvcrt;
    using pehost::msvcrt::File;
    using pehost::msvcrt::LConv;
    using pehost::msvcrt::g_iob;
    using pehost::msvcrt::g_lconv;
    using pehost::msvcrt::g_app_type;
    using pehost::msvcrt::ERR_INVAL;
    using pehost::msvcrt::ERR_BADF;
    using pehost::msvcrt::ERR_NOMEM;
    using pehost::msvcrt::ERR_RANGE;
    crt::reset();
    pehost::g_runtime = &g_recording;
    g_rec = Recorder{};
    expect(crt::__iob_func() == g_iob && &crt::__iob_func()[1] == &g_iob[1] && sizeof(File) == 48 && reinterpret_cast<u64>(&g_iob[1]) - reinterpret_cast<u64>(&g_iob[0]) == 48, "__iob_func returns three 48 byte stream records");
    expect(crt::fwrite("abc", 1, 3, &g_iob[1]) == 3 && g_rec.last_stream == 1 && same(g_rec.last_text, "abc") && crt::fwrite("abcdef", 2, 3, &g_iob[2]) == 3 && g_rec.last_stream == 2 && g_rec.last_length == 6, "fwrite writes size times count bytes to standard output or error");
    u32 writes = g_rec.writes;
    expect(crt::fwrite("x", 0, 5, &g_iob[1]) == 0 && crt::fwrite("x", 5, 0, &g_iob[1]) == 0 && g_rec.writes == writes, "a zero size or count writes nothing");
    expect(crt::fwrite("x", ~0ull, 2, &g_iob[1]) == 0 && *crt::_errno() == ERR_INVAL && g_rec.writes == writes, "a size times count that overflows is refused with EINVAL");
    File other{};
    *crt::_errno() = 0;
    expect(crt::fwrite("x", 1, 1, &other) == 0 && *crt::_errno() == ERR_BADF && crt::fwrite("x", 1, 1, &g_iob[0]) == 0 && crt::fputc('x', nullptr) == -1 && crt::fputs("x", &other) == -1 && g_rec.writes == writes,
           "a stream that is not standard output or error, including standard input, is EBADF");
    static char large[10000];
    for (u32 i = 0; i < sizeof(large); ++i) large[i] = 'z';
    g_rec = Recorder{};
    expect(crt::fwrite(large, 1, sizeof(large), &g_iob[1]) == sizeof(large) && g_rec.writes == 3 && g_rec.last_length == sizeof(large) - 8192, "a large write is split into chunks of at most 4096 bytes");
    expect(crt::fputc('A', &g_iob[1]) == 'A' && same(g_rec.last_text, "A") && crt::fputc(0x1FF, &g_iob[1]) == 0xFF && crt::fputs("hi", &g_iob[2]) == 0 && g_rec.last_stream == 2 && same(g_rec.last_text, "hi") && crt::putchar('Q') == 'Q' && same(g_rec.last_text, "Q"),
           "fputc, fputs and putchar");
    g_rec = Recorder{};
    expect(crt::puts("line") == 0 && g_rec.writes == 2 && g_rec.last_stream == 1 && same(g_rec.last_text, "\n"), "puts writes the text and a newline to standard output");
    expect(crt::fflush(nullptr) == 0 && crt::fflush(&g_iob[1]) == 0 && crt::fflush(&g_iob[2]) == 0 && crt::fflush(&g_iob[0]) == 0 && crt::fflush(&other) == -1, "fflush succeeds on the standard streams");

    g_rec = Recorder{};
    expect(crt::printf("v=%d", 42) == 4 && g_rec.last_stream == 1 && same(g_rec.last_text, "v=42") && crt::fprintf(&g_iob[2], "e%s", "rr") == 3 && g_rec.last_stream == 2, "printf and fprintf reach the console streams");
    *crt::_errno() = 0;
    expect(crt::printf("%f", 1) == -1 && *crt::_errno() == ERR_INVAL && crt::fprintf(&other, "x") == -1, "an unsupported conversion or stream makes printf fail");
    char buf[16];
    expect(crt::sprintf(buf, "%d-%s", 5, "ab") == 4 && same(buf, "5-ab") && crt::_snprintf(buf, 16, "%u", 9u) == 1 && same(buf, "9"), "sprintf and _snprintf");

    void* m = crt::malloc(100);
    expect(m != nullptr && pehost::g_world.heap.owns(m), "malloc allocates from the process heap");
    *crt::_errno() = 0;
    expect(crt::malloc(64u << 20) == nullptr && *crt::_errno() == ERR_NOMEM && crt::calloc(~0ull, 2) == nullptr && *crt::_errno() == ERR_NOMEM, "malloc past the cap and an overflowing calloc fail with ENOMEM");
    u8* c = static_cast<u8*>(crt::calloc(7, 9));
    bool zero = c != nullptr;
    for (u32 i = 0; zero && i < 63; ++i) zero = c[i] == 0;
    expect(zero && crt::calloc(0, 5) != nullptr && crt::calloc(5, 0) != nullptr, "calloc zeroes and accepts a zero count");
    for (u32 i = 0; i < 63; ++i) c[i] = 7;
    crt::free(c);
    u8* c2 = static_cast<u8*>(crt::calloc(7, 9));
    zero = c2 != nullptr;
    for (u32 i = 0; zero && i < 63; ++i) zero = c2[i] == 0;
    expect(zero, "calloc zeroes memory that was freed dirty");
    crt::free(nullptr);
    crt::free(&g_iob);
    void* r = crt::realloc(nullptr, 40);
    expect(r != nullptr && crt::realloc(r, 0) == nullptr && !pehost::g_world.heap.owns(r), "realloc of null allocates and a zero size frees");
    u8* r2 = static_cast<u8*>(crt::malloc(30));
    for (u32 i = 0; i < 30; ++i) r2[i] = static_cast<u8>(i);
    *crt::_errno() = 0;
    expect(crt::realloc(r2, 64u << 20) == nullptr && *crt::_errno() == ERR_NOMEM && pehost::g_world.heap.owns(r2) && r2[29] == 29, "a failed realloc leaves the old block valid");
    crt::free(r2);
    crt::free(m);

    u8 a[32], b[32];
    u32 mem_bad = 0;
    for (u32 round = 0; round < 20000; ++round) {
        for (u32 i = 0; i < 32; ++i) a[i] = b[i] = static_cast<u8>(below(4));
        u32 from = static_cast<u32>(below(16)), to = static_cast<u32>(below(16)), n = static_cast<u32>(below(16));
        crt::memmove(a + to, a + from, n);
        ::memmove(b + to, b + from, n);
        if (::memcmp(a, b, 32) != 0) ++mem_bad;
        u8 x[24], y[24];
        for (u32 i = 0; i < 24; ++i) x[i] = y[i] = static_cast<u8>(below(3));
        i32 got = crt::memcmp(x, y, 24), want = ::memcmp(x, y, 24);
        if ((got < 0) != (want < 0) || (got == 0) != (want == 0)) ++mem_bad;
        x[below(24)] ^= 1;
        got = crt::memcmp(x, y, 24);
        want = ::memcmp(x, y, 24);
        if (got != (want < 0 ? -1 : want > 0 ? 1 : 0)) ++mem_bad;
    }
    expect(mem_bad == 0, "memmove over both overlap directions and memcmp agree with libc on 20,000 random cases");
    crt::memset(a, 0x5A, 8);
    crt::memcpy(b, a, 8);
    expect(a[7] == 0x5A && a[8] != 0x5A && b[7] == 0x5A && crt::memmove(b, a, 0) == b && crt::memset(b, 1, 0) == b && crt::memcpy(b, a, 0) == b, "memset and memcpy and their zero length forms");

    u32 str_bad = 0;
    static const char* const words[] = { "", "a", "ab", "abc", "abcabc", "b", "hello", "hell", "help", "\xFF" };
    for (u32 i = 0; i < 10; ++i) {
        for (u32 k = 0; k < 10; ++k) {
            i32 got = crt::strcmp(words[i], words[k]), want = ::strcmp(words[i], words[k]);
            if ((got < 0) != (want < 0) || (got > 0) != (want > 0)) ++str_bad;
            for (u32 n = 0; n < 5; ++n) {
                got = crt::strncmp(words[i], words[k], n);
                want = ::strncmp(words[i], words[k], n);
                if ((got < 0) != (want < 0) || (got > 0) != (want > 0)) ++str_bad;
            }
            if (crt::strstr(words[i], words[k]) != ::strstr(words[i], words[k])) ++str_bad;
        }
        if (crt::strlen(words[i]) != __builtin_strlen(words[i])) ++str_bad;
        for (u32 ch = 0; ch < 256; ch += 1) {
            if (crt::strchr(words[i], static_cast<i32>(ch)) != ::strchr(words[i], static_cast<i32>(ch))) ++str_bad;
            if (crt::strrchr(words[i], static_cast<i32>(ch)) != ::strrchr(words[i], static_cast<i32>(ch))) ++str_bad;
        }
    }
    expect(str_bad == 0, "strcmp, strncmp, strstr, strlen, strchr and strrchr agree with libc on every pair of test words and every byte value");
    char s1[16] = "zzzzzzzzzzzzzzz";
    crt::strncpy(s1, "ab", 6);
    expect(s1[0] == 'a' && s1[1] == 'b' && s1[2] == 0 && s1[5] == 0 && s1[6] == 'z', "strncpy pads with zeros up to n and no further");
    crt::strncpy(s1, "abcdefgh", 4);
    expect(s1[3] == 'd' && s1[4] == 0, "strncpy does not terminate a truncated copy, as in C");
    crt::strcpy(s1, "xy");
    crt::strcat(s1, "zw");
    expect(same(s1, "xyzw") && crt::strcpy(s1, "") == s1 && crt::strlen(s1) == 0, "strcpy and strcat");
    static const u16 w3[] = { 'a', 'b', 'c', 0 };
    expect(crt::wcslen(w3) == 3 && crt::wcslen(w3 + 3) == 0 && crt::getenv("PATH") == nullptr, "wcslen and getenv");
    expect(same(crt::strerror(0), "No error") && same(crt::strerror(ERR_INVAL), "Invalid argument") && same(crt::strerror(ERR_BADF), "Bad file descriptor") && same(crt::strerror(ERR_NOMEM), "Not enough space") &&
               same(crt::strerror(ERR_RANGE), "Result too large") && same(crt::strerror(2), "No such file or directory") && same(crt::strerror(1), "Operation not permitted") && same(crt::strerror(9999), "Unknown error") && same(crt::strerror(-1), "Unknown error"),
           "strerror");
    LConv* lc = crt::localeconv();
    expect(lc == &g_lconv && same(lc->decimal_point, ".") && same(lc->thousands_sep, "") && lc->frac_digits == 127 && lc->w_decimal_point[0] == '.' && lc->w_decimal_point[1] == 0 && sizeof(LConv) == 10 * 8 + 8 + 8 * 8, "localeconv is the C locale");

    crt::reset();
    expect(crt::signal(2, reinterpret_cast<void*>(0x44)) == nullptr && crt::signal(2, nullptr) == reinterpret_cast<void*>(0x44) && crt::signal(15, reinterpret_cast<void*>(1)) == nullptr, "signal returns the previous handler per signal");
    *crt::_errno() = 0;
    expect(crt::signal(0, nullptr) == reinterpret_cast<void*>(~0ull) && crt::signal(3, nullptr) == reinterpret_cast<void*>(~0ull) && crt::signal(32, nullptr) == reinterpret_cast<void*>(~0ull) && crt::signal(-1, nullptr) == reinterpret_cast<void*>(~0ull) && *crt::_errno() == ERR_INVAL,
           "an unsupported signal number returns SIG_ERR with EINVAL");
    crt::reset();
    bool all_sigs = true;
    static const i32 sigs[] = { 2, 4, 8, 11, 15, 21, 22 };
    for (i32 sg : sigs) all_sigs = all_sigs && crt::signal(sg, reinterpret_cast<void*>(0x10)) == nullptr;
    expect(all_sigs, "SIGINT, SIGILL, SIGFPE, SIGSEGV, SIGTERM, SIGBREAK and SIGABRT are accepted");

    expect(crt::__setusermatherr(reinterpret_cast<void*>(8)) == nullptr && crt::__setusermatherr(nullptr) == reinterpret_cast<void*>(8), "__setusermatherr returns the previous handler");
    crt::__set_app_type(1);
    expect(g_app_type == 1 && crt::___lc_codepage_func() == 0 && crt::___mb_cur_max_func() == 1 && crt::__C_specific_handler(nullptr, nullptr, nullptr, nullptr) == 1, "start-up helpers");
    crt::_lock(8);
    crt::_unlock(8);

    g_rec = Recorder{};
    void* table[4] = { reinterpret_cast<void*>(&order_a), nullptr, reinterpret_cast<void*>(&order_a), reinterpret_cast<void*>(&order_a) };
    crt::_initterm(table, table + 4);
    expect(g_rec.writes == 3, "_initterm calls every non-null entry in the range");
    crt::_initterm(table, table);
    expect(g_rec.writes == 3, "and nothing for an empty range");
    void* failing[3] = { reinterpret_cast<void*>(&order_a), reinterpret_cast<void*>(&ms_probe_noop), reinterpret_cast<void*>(&order_a) };
    expect(crt::_initterm_e(failing, failing + 3) == 0 && g_rec.writes == 5, "_initterm_e runs everything when all succeed");
    void* stops[3] = { reinterpret_cast<void*>(&order_a), reinterpret_cast<void*>(&ms_five), reinterpret_cast<void*>(&order_a) };
    expect(crt::_initterm_e(stops, stops + 3) == 5 && g_rec.writes == 6, "and stops at the first non-zero result and returns it");

    i32 argc = 0;
    char** argv = nullptr;
    char** envp = nullptr;
    expect(crt::__getmainargs(&argc, &argv, &envp, 0, nullptr) == 0 && argc == 3 && same(argv[0], "C:\\dir\\prog.exe") && same(argv[1], "arg") && same(argv[2], "two words") && argv[3] == nullptr && envp != nullptr && envp[0] == nullptr,
           "__getmainargs splits the process command line and gives an empty environment");
    i32 argc2 = 0;
    char** argv2 = nullptr;
    crt::__getmainargs(&argc2, &argv2, &envp, 1, nullptr);
    expect(argc2 == 3 && argv2 == argv, "a second call gives the same result");
    crt::reset();
    pehost::g_runtime = &g_child_runtime;
    ChildResult e1 = in_child([] {
        pehost::msvcrt::reset();
        pehost::msvcrt::_onexit(reinterpret_cast<void*>(&ms_say_one));
        pehost::msvcrt::atexit(reinterpret_cast<void*>(&ms_say_two));
        pehost::msvcrt::exit(12);
    });
    expect(e1.exited && e1.code == 12 && same(e1.out, "two;one;"), "exit runs the registered handlers last to first and then ends the process with the code");
    ChildResult e2 = in_child([] {
        pehost::msvcrt::reset();
        pehost::msvcrt::atexit(reinterpret_cast<void*>(&ms_say_one));
        pehost::msvcrt::_exit(4);
    });
    expect(e2.exited && e2.code == 4 && same(e2.out, ""), "_exit skips the handlers");
    ChildResult e3 = in_child([] {
        pehost::msvcrt::reset();
        pehost::msvcrt::atexit(reinterpret_cast<void*>(&ms_say_one));
        pehost::msvcrt::_cexit();
        pehost::msvcrt::_cexit();
        pehost::msvcrt::exit(0);
    });
    expect(e3.exited && e3.code == 0 && same(e3.out, "one;"), "_cexit runs the handlers once and empties the list");
    ChildResult e4 = in_child([] { pehost::msvcrt::abort(); });
    expect(e4.exited && e4.code == 3 && contains(e4.out, "terminate it in an unusual way"), "abort writes its message and ends with status 3");
    ChildResult e5 = in_child([] { pehost::msvcrt::_amsg_exit(8); });
    expect(e5.exited && e5.code == 255 && contains(e5.out, "runtime error R6008\r\n"), "_amsg_exit names the runtime error and ends with status 255");
    ChildResult e6 = in_child([] { pehost::msvcrt::_amsg_exit(1234); });
    expect(e6.exited && e6.code == 255 && contains(e6.out, "R6234\r\n"), "a message number past three digits is reduced, not overflowed");
    pehost::g_runtime = &g_recording;
    crt::reset();
    bool full = true;
    for (u32 i = 0; i < 32; ++i) full = full && crt::_onexit(reinterpret_cast<void*>(&ms_say_one)) != nullptr;
    expect(full && crt::_onexit(reinterpret_cast<void*>(&ms_say_one)) == nullptr && crt::atexit(reinterpret_cast<void*>(&ms_say_one)) == -1 && crt::atexit(nullptr) == -1, "32 exit handlers fit, the 33rd is refused");
    crt::reset();
    for (u32 i = 0; i < 70; ++i) {
        pehost::g_world.command_line[i * 2] = 'a';
        pehost::g_world.command_line[i * 2 + 1] = ' ';
    }
    pehost::g_world.command_line[140] = 0;
    expect(crt::__getmainargs(&argc, &argv, &envp, 0, nullptr) == -1, "a command line with too many arguments makes __getmainargs fail");
}

}


PEHOST_WINAPI i32 snp(u64 options, char* buffer, u64 count, const char* f, ...) {
    pehost::fmt::VaList ap;
    __builtin_ms_va_start(ap, f);
    i32 r = pehost::ucrt::__stdio_common_vsprintf(options, buffer, count, f, nullptr, ap);
    __builtin_ms_va_end(ap);
    return r;
}

PEHOST_WINAPI i32 fpr(u64 options, pehost::msvcrt::File* fp, const char* f, ...) {
    pehost::fmt::VaList ap;
    __builtin_ms_va_start(ap, f);
    i32 r = pehost::ucrt::__stdio_common_vfprintf(options, fp, f, nullptr, ap);
    __builtin_ms_va_end(ap);
    return r;
}

u32 g_order[64];
u32 g_two_count = 0;
u32 g_order_count = 0;
u32 g_order_next = 0;
PEHOST_WINAPI void order_fn() { g_order[g_order_count++] = g_order_next++; }
PEHOST_WINAPI void order_two() { ++g_two_count; }

bool wide_is(const u16* w, const char* a) {
    for (u32 i = 0;; ++i) {
        if (w[i] != static_cast<u8>(a[i])) return false;
        if (a[i] == 0) return true;
    }
}

void ucrt_tests() {
    namespace u = pehost::ucrt;
    namespace crt = pehost::msvcrt;
    using namespace pehost::win32;
    using pe::modules::canonical;
    using pehost::set_last_error;
    using pehost::last_error;
    pehost::g_runtime = &g_recording;
    g_rec = Recorder{};
    pehost::World& w = pehost::g_world;
    w.heap.init(&test_map);
    w.imports = pehost::all_imports();

    expect(same(canonical("KERNEL32.DLL"), "kernel32.dll") && same(canonical("KernelBase.dll"), "kernel32.dll") && same(canonical("msvcrt.dll"), "msvcrt.dll") && same(canonical("UCRTBASE.dll"), "ucrtbase.dll"),
           "the real names of the three provided modules are canonical, case folded, with kernelbase folded into kernel32");
    expect(same(canonical("api-ms-win-crt-stdio-l1-1-0.dll"), "ucrtbase.dll") && same(canonical("API-MS-WIN-CRT-HEAP-L1-1-0.DLL"), "ucrtbase.dll") && same(canonical("api-ms-win-crt-.dll"), "ucrtbase.dll"),
           "every api-ms-win-crt set is the universal C runtime, in any case");
    expect(same(canonical("api-ms-win-core-synch-l1-2-0.dll"), "kernel32.dll") && same(canonical("API-MS-WIN-CORE-HANDLE-L1-1-0.DLL"), "kernel32.dll") && same(canonical("ext-ms-win-foo-l1-1-0.dll"), "kernel32.dll"),
           "the other api-ms-win and ext-ms-win sets are served by the kernel32 table");
    const char* plain[] = { "user32.dll", "", "dll", ".dll", "api-ms-win-crt-stdio", "api-ms-win-core.dl", "xapi-ms-win-core-l1.dll", "ucrtbase", "msvcrt.dll.x", "api-ms-crt-x.dll", "ext-ms-win" };
    bool same_ptr = true;
    for (const char* name : plain) same_ptr = same_ptr && canonical(name) == name;
    expect(same_ptr, "names that are not a known module or an api set come back unchanged as the same string");

    u64 address = 0;
    const pehost::ImportTable* all = pehost::all_imports();
    auto found = [&](const char* dll, const char* name) { return pehost::detail::resolve(const_cast<pehost::ImportTable*>(all), dll, name, 0, &address); };
    expect(found("api-ms-win-crt-heap-l1-1-0.dll", "malloc") && address == reinterpret_cast<u64>(&crt::malloc), "malloc is resolved through an api set to the same stub msvcrt uses");
    expect(found("api-ms-win-crt-stdio-l1-1-0.dll", "__stdio_common_vfprintf") && address == reinterpret_cast<u64>(&u::__stdio_common_vfprintf), "a UCRT-only function is resolved through its api set");
    expect(found("ucrtbase.dll", "__stdio_common_vsprintf") && found("UCRTBASE.DLL", "_configure_narrow_argv"), "the real ucrtbase name resolves in any case");
    expect(!found("msvcrt.dll", "__stdio_common_vfprintf") && !found("ucrtbase.dll", "__getmainargs") && !found("ucrtbase.dll", "printf") && !found("api-ms-win-crt-stdio-l1-1-0.dll", "__iob_func"),
           "each module only provides its own exports: msvcrt has no UCRT stdio and ucrtbase has no msvcrt start-up hooks");
    expect(found("api-ms-win-core-synch-l1-1-0.dll", "Sleep") && address == reinterpret_cast<u64>(&Sleep) && found("KERNELBASE.dll", "GetLastError") && address == reinterpret_cast<u64>(&GetLastError),
           "kernel32 functions are resolved through api-ms-win-core and kernelbase");
    expect(!found("api-ms-win-core-foo-l1-1-0.dll", "malloc") && !found("api-ms-win-crt-heap-l1-1-0.dll", "MALLOC") && !found("api-ms-win-crt-heap-l1-1-0.dll", "Sleep") && !found("user32.dll", "malloc"),
           "api sets keep the case sensitive function lookup and the module separation");

    u64 k32 = GetModuleHandleA("kernel32.dll");
    u64 ucrt = GetModuleHandleA("ucrtbase.dll");
    expect(ucrt != 0 && ucrt != k32 && ucrt != GetModuleHandleA("msvcrt.dll") && GetModuleHandleA("ucrtbase") == ucrt && GetModuleHandleA("api-ms-win-crt-stdio-l1-1-0.dll") == ucrt && GetModuleHandleA("KERNELBASE") == k32,
           "ucrtbase is a module with its own handle, reachable by its api set names, and kernelbase is kernel32");
    expect(LoadLibraryA("C:\\Windows\\System32\\UCRTBASE.DLL") == ucrt && LoadLibraryA("c:/windows/system32/kernel32") == k32 && LoadLibraryA("msvcrt.dll") == GetModuleHandleA("msvcrt.dll"),
           "LoadLibrary finds a built-in module by bare name, by name without extension and by a full path with either separator");
    set_last_error(0);
    expect(LoadLibraryA("nothere.dll") == 0 && last_error() == pehost::ERROR_MOD_NOT_FOUND && LoadLibraryA("") == 0 && LoadLibraryA("C:\\dir\\") == 0 && LoadLibraryA("a:b\\") == 0,
           "an unknown module, an empty name and a path ending in a separator are ERROR_MOD_NOT_FOUND");
    set_last_error(0);
    expect(LoadLibraryA(nullptr) == 0 && last_error() == pehost::ERROR_INVALID_PARAMETER && LoadLibraryW(nullptr) == 0, "a null name is ERROR_INVALID_PARAMETER");
    static const u16 wname[] = { 'U', 'C', 'R', 'T', 'B', 'A', 'S', 'E', 0 };
    static const u16 wmiss[] = { 'x', 'y', 0 };
    static u16 wlong[200];
    for (u32 i = 0; i < 199; ++i) wlong[i] = 'a';
    set_last_error(0);
    expect(LoadLibraryW(wname) == ucrt && LoadLibraryW(wmiss) == 0 && last_error() == pehost::ERROR_MOD_NOT_FOUND && LoadLibraryW(wlong) == 0, "LoadLibraryW matches like LoadLibraryA and refuses over-long names");
    set_last_error(0);
    expect(FreeLibrary(ucrt) == 1 && FreeLibrary(ucrt) == 1 && FreeLibrary(k32) == 1 && last_error() == 0, "FreeLibrary on a built-in module succeeds, repeatedly");
    expect(FreeLibrary(0) == 0 && last_error() == pehost::ERROR_INVALID_HANDLE && FreeLibrary(ucrt + 1) == 0, "FreeLibrary on a non-module is ERROR_INVALID_HANDLE");
    set_last_error(0);
    expect(GetProcAddress(ucrt, "malloc") == reinterpret_cast<void*>(&crt::malloc) && GetProcAddress(ucrt, "__stdio_common_vfprintf") == reinterpret_cast<void*>(&u::__stdio_common_vfprintf) &&
           GetProcAddress(GetModuleHandleA("msvcrt.dll"), "__stdio_common_vfprintf") == nullptr && GetProcAddress(ucrt, "__getmainargs") == nullptr && last_error() == pehost::ERROR_PROC_NOT_FOUND,
           "GetProcAddress on ucrtbase finds its own exports and not msvcrt's");

    u32 ucrt_count = 0, shared = 0, unique = 0;
    for (const pehost::ImportTable* t = all; t; t = t->next) {
        for (u32 i = 0; i < t->count; ++i) {
            if (!pehost::detail::same_dll(t->items[i].dll, "ucrtbase.dll")) continue;
            ++ucrt_count;
            bool also = false;
            for (const pehost::ImportTable* v = all; v; v = v->next) {
                for (u32 j = 0; j < v->count; ++j) {
                    if (pehost::detail::same_dll(v->items[j].dll, "msvcrt.dll") && pehost::detail::same_name(v->items[j].name, t->items[i].name)) also = also || v->items[j].address == t->items[i].address;
                }
            }
            if (also) ++shared;
            else ++unique;
        }
    }
    expect(ucrt_count == shared + unique && shared > 30 && unique > 30, "ucrtbase shares its C library stubs with msvcrt at the same addresses and adds its own");

    const char* line = "prog.exe \"two words\" caf\xC3\xA9";
    pehost::g_world.command_line[0] = 0;
    {
        u32 n = 0;
        while (line[n]) {
            w.command_line[n] = line[n];
            ++n;
        }
        w.command_line[n] = 0;
    }
    u::reset();
    expect(*u::__p___argc() == 3 && (*u::__p___argv())[1][0] == 't' && same((*u::__p___argv())[1], "two words") && same((*u::__p___argv())[2], "caf\xC3\xA9") && (*u::__p___argv())[3] == nullptr,
           "__p___argc and __p___argv parse the command line on first use");
    u::reset();
    expect(u::_configure_narrow_argv(1) == 0 && *u::__p___argc() == 3 && u::_configure_narrow_argv(2) == 0 && *u::__p___argc() == 3, "_configure_narrow_argv in either mode fills argc and argv, and repeating it is harmless");
    u::reset();
    expect(u::_configure_wide_argv(1) == 0 && wide_is((*u::__p___wargv())[0], "prog.exe") && wide_is((*u::__p___wargv())[1], "two words") && (*u::__p___wargv())[2][2] == 'f' && (*u::__p___wargv())[2][3] == 0xE9 && (*u::__p___wargv())[2][4] == 0 &&
           (*u::__p___wargv())[3] == nullptr && *u::__p___argc() == 3,
           "the wide argument vector is the same arguments in UTF-16 and ends with a null");
    u::reset();
    u16** wv = *u::__p___wargv();
    expect(wv != nullptr && wide_is(wv[1], "two words") && *u::__p___argc() == 3, "__p___wargv parses on first use");
    expect(u::_configure_narrow_argv(3) == crt::ERR_INVAL && u::_configure_narrow_argv(-1) == crt::ERR_INVAL && u::_configure_wide_argv(3) == crt::ERR_INVAL && u::_configure_wide_argv(-1) == crt::ERR_INVAL, "a mode outside 0 to 2 is EINVAL");
    u::reset();
    expect(u::_configure_narrow_argv(0) == 0 && *u::__p___argc() == 0 && (*u::__p___argv())[0] == nullptr, "mode 0 means no arguments");
    u::reset();
    expect(u::_configure_wide_argv(0) == 0 && *u::__p___argc() == 0 && (*u::__p___wargv())[0] == nullptr, "mode 0 for the wide vector means no arguments either");
    for (u32 i = 0; i < 70; ++i) {
        w.command_line[i * 2] = 'a';
        w.command_line[i * 2 + 1] = ' ';
    }
    w.command_line[140] = 0;
    u::reset();
    expect(u::_configure_narrow_argv(1) == crt::ERR_NOMEM && u::_configure_wide_argv(1) == crt::ERR_NOMEM, "a command line with too many arguments is ENOMEM");
    w.command_line[0] = 0;
    u::reset();
    expect(u::_configure_narrow_argv(1) == 0 && *u::__p___argc() == 0 && (*u::__p___argv())[0] == nullptr && u::_configure_wide_argv(1) == 0 && (*u::__p___wargv())[0] == nullptr, "an empty command line has no arguments");

    expect(*u::__p__environ() == crt::g_environment && **u::__p__environ() == nullptr && u::_get_initial_narrow_environment() == crt::g_environment && *u::__p__wenviron() != nullptr && **u::__p__wenviron() == nullptr &&
           u::_get_initial_wide_environment() == *u::__p__wenviron() && u::_initialize_narrow_environment() == 0 && u::_initialize_wide_environment() == 0,
           "the environment is empty in both widths");

    u::reset();
    expect(u::__p__fmode() == &crt::g_fmode && u::__p__commode() == &crt::g_commode && u::_set_fmode(0x4000) == 0 && *u::__p__fmode() == 0x4000, "the mode variables are the msvcrt ones");
    i32 got = 0;
    expect(u::_get_fmode(&got) == 0 && got == 0x4000 && u::_set_fmode(0x8000) == 0 && u::_get_fmode(&got) == 0 && got == 0x8000 && u::_set_fmode(0) == 0, "_set_fmode and _get_fmode round trip text, binary and default");
    expect(u::_set_fmode(5) == crt::ERR_INVAL && *u::__p__fmode() == 0 && u::_get_fmode(nullptr) == crt::ERR_INVAL, "an invalid mode or a null destination is EINVAL");
    u::_set_app_type(2);
    expect(crt::g_app_type == 2 && u::_set_new_mode(1) == 0 && u::_set_new_mode(0) == 1 && u::_set_new_mode(2) == -1 && u::_set_new_mode(-1) == -1 && u::_set_new_mode(0) == 0, "_set_app_type and _set_new_mode keep their values");
    expect(u::_set_invalid_parameter_handler(reinterpret_cast<void*>(0x1234)) == nullptr && u::_set_invalid_parameter_handler(nullptr) == reinterpret_cast<void*>(0x1234) && u::_configthreadlocale(1) == 0,
           "the invalid parameter handler is stored and the old one returned");
    expect(u::__acrt_iob_func(0) == &crt::g_iob[0] && u::__acrt_iob_func(1) == &crt::g_iob[1] && u::__acrt_iob_func(2) == &crt::g_iob[2] && u::__acrt_iob_func(3) == nullptr && u::__acrt_iob_func(~0u) == nullptr,
           "__acrt_iob_func hands out the three standard streams");

    g_rec = Recorder{};
    expect(fpr(0, &crt::g_iob[1], "n=%d %s", 12, "ok") == 7 && g_rec.last_stream == 1 && same(g_rec.last_text, "n=12 ok") && fpr(4, &crt::g_iob[2], "e") == 1 && g_rec.last_stream == 2, "__stdio_common_vfprintf prints to the standard streams whatever the options");
    *crt::_errno() = 0;
    expect(fpr(0, &crt::g_iob[0], "x") == -1 && *crt::_errno() == crt::ERR_BADF && fpr(0, &crt::g_iob[1], "%f", 1) == -1 && *crt::_errno() == crt::ERR_INVAL, "an unusable stream or an unsupported conversion fails");
    expect(u::__stdio_common_vfwprintf(0, &crt::g_iob[1], nullptr, nullptr, pehost::fmt::VaList{}) == -1, "wide printf reports failure");

    char buf[16];
    auto fill = [&]() {
        for (u32 i = 0; i < 16; ++i) buf[i] = 'Z';
    };
    fill();
    expect(snp(2, buf, 4, "%s", "hello") == 5 && same(buf, "hel") && buf[4] == 'Z', "standard snprintf truncates, terminates and returns the full length");
    fill();
    expect(snp(2, buf, 6, "%s", "hello") == 5 && same(buf, "hello") && buf[6] == 'Z', "standard snprintf that fits is terminated");
    fill();
    expect(snp(2, buf, 5, "%s", "hello") == 5 && same(buf, "hell") && buf[5] == 'Z', "standard snprintf of exactly the buffer size loses the last character to the terminator");
    fill();
    expect(snp(2, buf, 0, "%s", "hello") == 5 && buf[0] == 'Z', "standard snprintf with size 0 only counts");
    fill();
    expect(snp(3, buf, 1, "ab") == 2 && buf[0] == 0 && buf[1] == 'Z', "other option bits do not change the standard result");
    fill();
    expect(snp(0, buf, 4, "%s", "hello") == -1 && buf[0] == 'h' && buf[3] == 'l' && buf[4] == 'Z', "legacy truncation returns -1 and writes exactly count characters");
    fill();
    expect(snp(1, buf, 5, "%s", "hello") == 5 && buf[4] == 'o' && buf[5] == 'Z', "legacy output that exactly fills the buffer has no terminator");
    fill();
    expect(snp(0, buf, 6, "%s", "hello") == 5 && same(buf, "hello"), "legacy output that fits is terminated");
    fill();
    expect(snp(0, buf, ~0ull, "%d", 42) == 2 && same(buf, "42"), "an unbounded count behaves like sprintf");
    *crt::_errno() = 0;
    fill();
    expect(snp(2, buf, 8, "%f", 1) == -1 && buf[0] == 0 && *crt::_errno() == crt::ERR_INVAL, "a failing conversion returns -1 with an empty string");
    fill();
    expect(snp(0, buf, 0, "%f", 1) == -1 && buf[0] == 'Z', "a failing conversion with no buffer writes nothing");

    crt::reset();
    g_rec = Recorder{};
    expect(u::_crt_atexit(reinterpret_cast<void*>(&ms_say_one)) == 0, "_crt_atexit registers");
    crt::_cexit();
    expect(same(g_rec.last_text, "one;"), "an exit handler registered with _crt_atexit runs at _cexit");
    bool full = true;
    for (u32 i = 0; i < 32; ++i) full = full && u::_crt_atexit(reinterpret_cast<void*>(&ms_say_one)) == 0;
    expect(full && u::_crt_atexit(reinterpret_cast<void*>(&ms_say_one)) == -1, "_crt_atexit shares the 32 slots of atexit");
    crt::reset();
    full = true;
    for (u32 i = 0; i < 32; ++i) full = full && u::_crt_at_quick_exit(reinterpret_cast<void*>(&ms_say_one)) == 0;
    expect(full && u::_crt_at_quick_exit(reinterpret_cast<void*>(&ms_say_one)) == -1, "_crt_at_quick_exit has 32 slots");
    u::reset();
    expect(u::_crt_at_quick_exit(nullptr) == 0, "reset empties the quick exit list");

    u::OnexitTable table{ reinterpret_cast<void**>(1), reinterpret_cast<void**>(2), reinterpret_cast<void**>(3) };
    expect(u::_initialize_onexit_table(&table) == 0 && table.first == nullptr && table.last == nullptr && table.end == nullptr && u::_initialize_onexit_table(nullptr) == -1 &&
           u::_register_onexit_function(nullptr, nullptr) == -1 && u::_execute_onexit_table(nullptr) == -1,
           "an onexit table starts empty and a null table is refused");
    g_order_count = g_order_next = 0;
    for (u32 i = 0; i < 20; ++i) {
        void* fn = i == 3 ? nullptr : reinterpret_cast<void*>(&order_fn);
        if (u::_register_onexit_function(&table, fn) != 0) g_failures++;
    }
    expect(table.last - table.first == 20 && table.end - table.first == 32 && w.heap.owns(table.first), "the table grows from 8 slots by doubling");
    expect(u::_execute_onexit_table(&table) == 0 && g_order_count == 19 && g_order[0] == 0 && g_order[18] == 18 && table.first == nullptr && table.last == nullptr && table.end == nullptr, "the table runs its non-null entries, last registered first, and empties itself");
    u32 before = g_order_count;
    expect(u::_execute_onexit_table(&table) == 0 && g_order_count == before && u::_register_onexit_function(&table, reinterpret_cast<void*>(&order_two)) == 0 && (g_two_count = 0, u::_execute_onexit_table(&table) == 0) && g_two_count == 1, "an emptied table can be used again");

    g_two_count = 0;
    u32 registered = 0;
    while (registered < 4000000 && u::_register_onexit_function(&table, reinterpret_cast<void*>(&order_two)) == 0) ++registered;
    expect(registered < 4000000 && registered >= 8 && table.last - table.first == registered && table.last <= table.end, "registration fails cleanly when the heap cannot grow the table");
    void** keep = table.first;
    expect(u::_register_onexit_function(&table, reinterpret_cast<void*>(&order_two)) == -1 && table.first == keep && table.last - table.first == registered, "a failed registration leaves the table as it was");
    expect(u::_execute_onexit_table(&table) == 0 && g_two_count == registered, "and every registered entry still runs");

    u16 cw = 0;
    u16 odd = 0x27F;
    asm volatile("fldcw %0" : : "m"(odd));
    u::_fpreset();
    asm volatile("fnstcw %0" : "=m"(cw));
    expect(cw == 0x37F, "_fpreset restores the default x87 control word");

    expect(*u::__daylight() == 0 && *u::__timezone() == 0 && same(u::__tzname()[0], "UTC") && same(u::__tzname()[1], "UTC"), "time zone data is UTC");
    u::_tzset();
    expect(*u::__daylight() == 0 && *u::__timezone() == 0, "_tzset changes nothing");
    u::_c_exit();
}

int main(int argc, char** argv) {
    g_quick = argc > 1 && argv[1][0] == '-' && argv[1][1] == 'q';
    heap_tests();
    codec_tests();
    format_tests();
    cmdline_tests();
    heap_edge_tests();
    cmdline_edge_tests();
    kernel32_tests();
    kernel32_edge_tests();
    msvcrt_tests();
    msvcrt_edge_tests();
    ucrt_tests();
    printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
