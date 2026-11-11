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

    {
        struct JoinCase {
            const char* argv[6];
            u32 argc;
        };
        const JoinCase joins[] = {
            { { "prog", "a", "b c" }, 3 },
            { { "p", "", "x" }, 3 },
            { { "p", "a\"b" }, 2 },
            { { "p", "back\\" }, 2 },
            { { "p", "back slash\\" }, 2 },
            { { "p", "a\\\\\"b" }, 2 },
            { { "my prog", "x y" }, 2 },
            { { "p", "\\\\server\\share dir\\", "z" }, 3 },
            { { "p", "tab\there" }, 2 },
            { { "p", "\"\"" }, 2 },
            { { "p", "\\", "\\\\", "\\\"" }, 4 },
            { { "only" }, 1 },
        };
        u32 round = 0;
        for (const JoinCase& c : joins) {
            char line[256];
            char store[256];
            char* parsed[16];
            u32 count = 0;
            bool ok = pehost::cmdline::join(c.argv, c.argc, line, sizeof(line)) && pehost::cmdline::parse(line, store, sizeof(store), parsed, 16, &count) && count == c.argc;
            for (u32 i = 0; ok && i < c.argc; ++i) ok = same(parsed[i], c.argv[i]);
            if (ok) ++round;
            else printf("join round trip failed for [%s]\n", c.argv[0]);
        }
        expect(round == sizeof(joins) / sizeof(joins[0]), "joining arguments into a command line and parsing it again gives back the same arguments, including empty, quoted and backslash cases");
        const char* two[] = { "p", "a b" };
        const char* empty[] = { "p", "" };
        const char* quote[] = { "p", "a\"b" };
        char line[64];
        expect(pehost::cmdline::join(two, 2, line, sizeof(line)) && same(line, "p \"a b\""), "an argument with a space is quoted");
        expect(pehost::cmdline::join(empty, 2, line, sizeof(line)) && same(line, "p \"\""), "an empty argument is written as two quotes");
        expect(pehost::cmdline::join(quote, 2, line, sizeof(line)) && same(line, "p \"a\\\"b\""), "a quote inside an argument is escaped");
        const char* plain[] = { "a", "b" };
        expect(pehost::cmdline::join(plain, 2, line, 4) && same(line, "a b") && !pehost::cmdline::join(plain, 2, line, 3), "a line that exactly fills the buffer with its terminator fits and one byte less does not");
        char guard[8];
        for (u32 i = 0; i < sizeof(guard); ++i) guard[i] = 'g';
        expect(!pehost::cmdline::join(two, 2, guard, 4) && guard[4] == 'g' && guard[5] == 'g', "a refused join does not write past the capacity");
        expect(pehost::cmdline::join(nullptr, 0, line, sizeof(line)) && line[0] == 0, "no arguments is an empty line");
    }
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


char g_keybuf[64];
char g_spellbuf[64];

u32 chain_img_rva_add3(const u8* file, u64 size) {
    static pe::Image img;
    pe::parse(file, size, &img);
    u8* flat = map_pages(img.size_of_image);
    pe::map(img, file, size, flat, img.size_of_image);
    pe::ExportRef r;
    pe::find_export(flat, img.size_of_image, img.directories[pe::DIR_EXPORT], "add3", 0, &r);
    return r.rva;
}

u8* read_sample(const char* name, u64* size) {
    char path[160];
    const char* dir = "tools/pe-samples/bin/";
    u32 n = 0;
    for (const char* p = dir; *p; ++p) path[n++] = *p;
    for (const char* p = name; *p && n + 1 < sizeof(path); ++p) path[n++] = *p;
    path[n] = 0;
    i64 fd = lx(2, reinterpret_cast<u64>(path), 0);
    if (fd < 0) return nullptr;
    i64 end = lx(8, static_cast<u64>(fd), 0, 2);
    u8* out = end > 0 ? map_pages(static_cast<u64>(end) + 1) : nullptr;
    bool ok = out && lx(17, static_cast<u64>(fd), reinterpret_cast<u64>(out), static_cast<u64>(end), 0) == end;
    lx(SYS_CLOSE, static_cast<u64>(fd));
    if (!ok) return nullptr;
    *size = static_cast<u64>(end);
    return out;
}

struct Tracked {
    u8* at;
    u64 bytes;
};
Tracked g_tracked[64];
u32 g_stray = 0;
void track(u8* at, u64 bytes) {
    if (!at) return;
    for (Tracked& t : g_tracked) {
        if (!t.at) {
            t.at = at;
            t.bytes = (bytes + 4095) / 4096 * 4096;
            return;
        }
    }
}
u32 g_unmapped = 0;
u64 g_truncate_to = 0;
u8* mod_read(const char* name, u64* size) {
    const char* real = name;
    if (name[0] == 'p' && name[1] >= '0' && name[1] <= '9') real = "dllplug.dll";
    u8* bytes = read_sample(real, size);
    if (bytes) track(bytes, *size + 1);
    if (bytes && g_truncate_to && g_truncate_to < *size) *size = g_truncate_to;
    return bytes;
}
void mod_unmap(u8* at, u64 bytes) {
    ++g_unmapped;
    bool known = false;
    for (Tracked& t : g_tracked) {
        if (t.at == at && t.at && bytes <= t.bytes) {
            t.at = nullptr;
            known = true;
        }
    }
    if (!known) {
        ++g_stray;
        return;
    }
    lx(SYS_MUNMAP, reinterpret_cast<u64>(at), bytes);
}
u8* mod_reserve(u64 bytes) {
    u8* at = map_pages(bytes);
    track(at, bytes);
    return at;
}
bool mod_protect(u8*, u64, u32) { return true; }
pehost::Runtime g_modrt = { &rec_exit, &rec_write, &mod_reserve, &mod_protect, &rec_sleep, &mod_read, &mod_unmap };
bool real_protect(u8* at, u64 bytes, u32 prot) { return lx(10, reinterpret_cast<u64>(at), bytes, prot) == 0; }
pehost::Runtime g_childmod = { &child_exit_process, &child_console, &mod_reserve, &real_protect, &rec_sleep, &mod_read, &mod_unmap };

void child_load_plug() {
    namespace m = pehost::modules;
    pehost::g_runtime = &g_childmod;
    static u64 tls[64];
    m::reset(pehost::all_imports());
    m::install_tls(tls);
    m::DirectCall call;
    u32 e = 0;
    m::load_library("dllplug.dll", call, &e);
}

void module_tests() {
    namespace m = pehost::modules;
    pehost::g_runtime = &g_modrt;
    static u64 tls_array[64];
    m::reset(pehost::all_imports());
    m::install_tls(tls_array);
    m::NoCall none;
    u32 err = 0;

    u64 file_size = 0;
    u8* chain_file = read_sample("dllchain.dll", &file_size);
    static pe::Image chain_img;
    pe::parse(chain_file, file_size, &chain_img);
    u8* flat = map_pages(chain_img.size_of_image);
    pe::map(chain_img, chain_file, file_size, flat, chain_img.size_of_image);
    pe::ExportRef ref;
    pe::Directory dir = chain_img.directories[pe::DIR_EXPORT];
    using R = pe::ExportResult;
    expect(pe::find_export(flat, chain_img.size_of_image, dir, "add3", 0, &ref) == R::Found && !ref.forwarded && ref.rva != 0 && ref.rva < chain_img.size_of_image, "an export is found by name");
    expect(pe::find_export(flat, chain_img.size_of_image, dir, nullptr, 9, &ref) == R::Found && !ref.forwarded && ref.rva != 0, "an export is found by its ordinal, which starts at the directory's base, not at 1");
    expect(pe::find_export(flat, chain_img.size_of_image, dir, "fwd_add", 0, &ref) == R::Found && ref.forwarded && same(ref.forward, "dllmath.add") &&
           pe::find_export(flat, chain_img.size_of_image, dir, nullptr, 8, &ref) == R::Found && ref.forwarded && same(ref.forward, "KERNEL32.Sleep"),
           "a forwarder yields its target text, by name or by ordinal");
    expect(pe::find_export(flat, chain_img.size_of_image, dir, "ADD3", 0, &ref) == R::Missing && pe::find_export(flat, chain_img.size_of_image, dir, "add", 0, &ref) == R::Missing &&
           pe::find_export(flat, chain_img.size_of_image, dir, "add33", 0, &ref) == R::Missing, "names are case sensitive and must match whole");
    expect(pe::find_export(flat, chain_img.size_of_image, dir, nullptr, 5, &ref) == R::Missing && pe::find_export(flat, chain_img.size_of_image, dir, nullptr, 10, &ref) == R::Missing &&
           pe::find_export(flat, chain_img.size_of_image, dir, nullptr, 0, &ref) == R::Missing && pe::find_export(flat, chain_img.size_of_image, dir, nullptr, 0xFFFF, &ref) == R::Missing,
           "ordinals below the base, past the table and zero are missing");
    expect(pe::find_export(flat, chain_img.size_of_image, pe::Directory{ 0, 0 }, "add3", 0, &ref) == R::Missing && pe::find_export(flat, chain_img.size_of_image, pe::Directory{ dir.rva, 39 }, "add3", 0, &ref) == R::Bad &&
           pe::find_export(flat, chain_img.size_of_image, pe::Directory{ chain_img.size_of_image - 8, 40 }, "add3", 0, &ref) == R::Bad,
           "no export directory is missing and a truncated or out of range one is bad");

    u32 bad = 0, found_wild = 0;
    u8* work = map_pages(chain_img.size_of_image);
    for (u32 round = 0; round < (g_quick ? 3000u : 40000u); ++round) {
        for (u32 i = 0; i < chain_img.size_of_image; ++i) work[i] = flat[i];
        u32 edits = 1 + static_cast<u32>(below(6));
        for (u32 e = 0; e < edits; ++e) {
            u32 at = dir.rva + static_cast<u32>(below(dir.size + 64));
            if (at < chain_img.size_of_image) work[at] = static_cast<u8>(below(256));
        }
        pe::Directory d = dir;
        if (below(4) == 0) d.rva += static_cast<u32>(below(48)) - 24;
        if (below(4) == 0) d.size = static_cast<u32>(below(dir.size * 2));
        const char* name = below(3) == 0 ? nullptr : (below(2) ? "add3" : "fwd_sleep");
        pe::ExportResult r = pe::find_export(work, chain_img.size_of_image, d, name, static_cast<u32>(below(14)), &ref);
        if (r == R::Found) {
            ++found_wild;
            if (!ref.forwarded && ref.rva >= chain_img.size_of_image) ++bad;
            if (ref.forwarded && ref.forward[0] == 0) ++bad;
        }
    }
    expect(bad == 0 && found_wild > 100, "40,000 random corruptions of the export directory never give an address outside the image or an empty forwarder (and many still resolve)");

    expect(m::normalise("C:\\Win\\System32\\MyLib.DLL", g_keybuf, g_spellbuf) && same(g_keybuf, "mylib.dll") && same(g_spellbuf, "MyLib.DLL") && m::normalise("a/b/plug", g_keybuf, g_spellbuf) && same(g_keybuf, "plug.dll") && same(g_spellbuf, "plug.dll"),
           "module names reduce to the file name, a missing extension becomes .dll, and the spelling is kept for the file lookup");
    expect(!m::normalise("", g_keybuf, g_spellbuf) && !m::normalise("dir\\", g_keybuf, g_spellbuf) && !m::normalise("0123456789012345678901234567890123456789012345678901234567890123.dll", g_keybuf, g_spellbuf) && m::normalise("v.x", g_keybuf, g_spellbuf) && same(g_keybuf, "v.x"),
           "an empty name, a directory and a name that is too long are refused, and a name with its own extension keeps it");

    err = 0;
    u64 chain = m::load_library("dllchain.dll", none, &err);
    i32 ci = m::find("dllchain.dll");
    i32 mi = m::find("dllmath.dll");
    expect(chain != 0 && ci >= 0 && mi >= 0 && m::g_state.items[ci].refs == 1 && m::g_state.items[mi].refs == 1 && m::g_state.items[ci].attached && m::g_state.items[mi].attached,
           "loading a DLL loads what it imports, holds one reference on each and marks both initialised");
    expect(m::g_state.order_count == 2 && m::g_state.order[0] == mi && m::g_state.order[1] == ci, "a dependency is initialised before the DLL that imports it");
    expect(chain % 0x10000 == 0 && m::find_base(chain) == ci && reinterpret_cast<u64>(m::g_state.items[mi].base) % 0x10000 == 0, "the image bases are 64 KiB aligned");
    expect(m::g_state.items[ci].tls_index >= 1 && m::g_state.items[mi].tls_index >= 1 && m::g_state.items[ci].tls_index != m::g_state.items[mi].tls_index &&
           tls_array[m::g_state.items[ci].tls_index] == reinterpret_cast<u64>(m::g_state.items[ci].tls_block) && tls_array[m::g_state.items[mi].tls_index] != 0 &&
           tls_array[0] == 0,
           "each DLL with thread-local storage gets its own index above 0 and its block in the thread's table");
    u32 idx_value = 0;
    for (u32 k = 0; k < 4; ++k) idx_value |= static_cast<u32>(m::g_state.items[ci].base[m::g_state.items[ci].tls.index_rva + k]) << (8 * k);
    expect(idx_value == m::g_state.items[ci].tls_index, "the DLL's own TLS index variable holds the assigned index");

    u64 address = 0;
    const pehost::ImportTable* builtins = pehost::all_imports();
    expect(m::export_address(builtins, ci, "add3", 0, 0, &address) && address == chain + chain_img_rva_add3(chain_file, file_size),
           "a plain export is the image base plus its relative address");
    u64 math_add = 0;
    expect(m::export_address(builtins, mi, "add", 0, 0, &math_add) && m::export_address(builtins, ci, "fwd_add", 0, 0, &address) && address == math_add && m::export_address(builtins, ci, nullptr, 7, 0, &address) && address == math_add,
           "a forwarder to another loaded DLL is that DLL's own address, by name or by ordinal");
    expect(m::export_address(builtins, ci, "fwd_sleep", 0, 0, &address) && address == reinterpret_cast<u64>(&pehost::win32::Sleep), "a forwarder into kernel32 is the built-in stub");
    expect(!m::export_address(builtins, ci, "nothere", 0, 0, &address) && !m::export_address(builtins, ci, "fwd_add", 0, m::MAX_HOPS, &address), "an unknown name fails and a forwarder is not followed past the hop limit");

    u8* base = m::g_state.items[ci].base;
    pe::ExportRef fwd;
    pe::find_export(base, chain_img.size_of_image, chain_img.directories[pe::DIR_EXPORT], "fwd_add", 0, &fwd);
    char* text = reinterpret_cast<char*>(base + 0);
    (void)text;
    u8* image = const_cast<u8*>(base);
    pe::Directory live = m::g_state.items[ci].exports;
    auto forward_rva = [&]() {
        u32 first = *reinterpret_cast<u32*>(image + live.rva + 28);
        return *reinterpret_cast<u32*>(image + first + 4);
    };
    u32 where = forward_rva();
    auto patch = [&](const char* to) {
        u32 i = 0;
        for (; to[i]; ++i) image[where + i] = static_cast<u8>(to[i]);
        image[where + i] = 0;
    };
    patch("dllmath.#2");
    u64 count_fn = 0;
    expect(m::export_address(builtins, mi, "call_count", 0, 0, &count_fn) && m::export_address(builtins, ci, "fwd_add", 0, 0, &address) && address == count_fn, "a forwarder by ordinal (name.#N) reaches the export with that ordinal");
    patch("dllmath.#");
    expect(!m::export_address(builtins, ci, "fwd_add", 0, 0, &address), "a forwarder with an empty ordinal fails");
    patch("dllmath.#70000");
    expect(!m::export_address(builtins, ci, "fwd_add", 0, 0, &address), "a forwarder ordinal above 65535 fails");
    patch("dllmath.#2x");
    expect(!m::export_address(builtins, ci, "fwd_add", 0, 0, &address), "a forwarder ordinal with a letter fails");
    patch("nodot");
    expect(!m::export_address(builtins, ci, "fwd_add", 0, 0, &address), "a forwarder with no dot fails");
    patch(".add");
    expect(!m::export_address(builtins, ci, "fwd_add", 0, 0, &address), "a forwarder with no module name fails");
    patch("dllmath.");
    expect(!m::export_address(builtins, ci, "fwd_add", 0, 0, &address), "a forwarder with no function name fails");
    patch("dllchain.fwd_add");
    expect(!m::export_address(builtins, ci, "fwd_add", 0, 0, &address), "a forwarder that points at itself ends at the hop limit instead of looping");
    patch("nosuch.add");
    u32 modules_before = 0;
    for (u32 i = 0; i < m::MAX_MODULES; ++i) modules_before += m::g_state.items[i].used;
    expect(!m::export_address(builtins, ci, "fwd_add", 0, 0, &address), "a forwarder into a DLL that does not exist fails");
    patch("dllmath.add");

    u32 before = g_unmapped;
    u64 again = m::load_library("DLLCHAIN", none, &err);
    expect(again == chain && m::g_state.items[ci].refs == 2 && m::g_state.items[mi].refs == 1 && g_unmapped == before, "loading it again by another spelling adds a reference to it only");
    m::release(ci, none);
    expect(m::g_state.items[ci].used && m::g_state.items[ci].refs == 1, "releasing once leaves it loaded");
    m::release(ci, none);
    expect(!m::g_state.items[ci].used && !m::g_state.items[mi].used && m::find("dllchain.dll") < 0 && g_unmapped > before && tls_array[1] == 0 && tls_array[2] == 0,
           "releasing the last reference unloads it and, through it, its dependency, with their memory and TLS entries");
    expect(g_stray == 0, "everything a module manager unmaps is exactly something it was given and never any other memory");
    u32 mapped_ok = 0;
    for (u32 i = 0; i < m::MAX_MODULES; ++i) mapped_ok += m::g_state.items[i].used;
    expect(mapped_ok == 0 && m::g_state.order_count == 0, "nothing is left loaded or initialised");

    err = 0;
    expect(m::load_library("nosuch.dll", none, &err) == 0 && err == m::ERROR_NOT_FOUND, "an unknown DLL is ERROR_MOD_NOT_FOUND");
    err = 0;
    expect(m::load_library("dyn.exe", none, &err) == 0 && err == m::ERROR_BAD_IMAGE, "a program is not loadable as a library: ERROR_BAD_EXE_FORMAT");
    g_truncate_to = 600;
    err = 0;
    expect(m::load_library("dllplug.dll", none, &err) == 0 && err == m::ERROR_BAD_IMAGE, "a truncated DLL file is ERROR_BAD_EXE_FORMAT");
    g_truncate_to = 0;
    u32 live_slots = 0;
    for (u32 i = 0; i < m::MAX_MODULES; ++i) live_slots += m::g_state.items[i].used;
    expect(live_slots == 0, "failed loads leave nothing behind");

    pehost::g_runtime = &g_recording;
    m::reset(pehost::all_imports());
    ChildResult killed = in_child([] {
        child_load_plug();
        pehost::win32::TerminateProcess(~0ull, 9);
    });
    ChildResult ended = in_child([] {
        child_load_plug();
        pehost::win32::ExitProcess(9);
    });
    expect(killed.exited && killed.code == 9 && contains(killed.out, "dllplug reason=1") && !contains(killed.out, "reason=0") &&
           ended.exited && ended.code == 9 && contains(ended.out, "dllplug reason=1") && contains(ended.out, "dllplug reason=0 reserved=1"),
           "ExitProcess detaches the loaded DLLs with a non-null reserved argument and TerminateProcess on the current process does not, as on Windows");
}

struct FakeFile {
    const char* name;
    const u8* data;
    u64 size;
};

u8 g_fake_pattern[300];
const u8 g_fake_hello[] = { 'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd' };
FakeFile g_fake_files[] = {
    { "a.txt", g_fake_hello, 11 },
    { "b.dat", g_fake_pattern, 300 },
};
u32 g_fake_closed = 0;
u32 g_fake_opened = 0;
u64 g_fake_chunk = 0;
u64 g_fake_fail_from = ~0ull;
u64 g_fake_last_offset = 0;

i64 fake_open(const char* name, u64* size) {
    for (i64 i = 0; i < 2; ++i) {
        if (same(g_fake_files[i].name, name)) {
            ++g_fake_opened;
            *size = g_fake_files[i].size;
            return 500 + i;
        }
    }
    return -1;
}

i64 fake_read(i64 handle, u64 offset, void* buffer, u64 length) {
    const FakeFile& f = g_fake_files[handle - 500];
    g_fake_last_offset = offset;
    if (offset >= g_fake_fail_from) return -5;
    if (offset >= f.size) return 0;
    u64 n = f.size - offset;
    if (n > length) n = length;
    if (g_fake_chunk && n > g_fake_chunk) n = g_fake_chunk;
    if (g_fake_fail_from != ~0ull && offset + n > g_fake_fail_from) n = g_fake_fail_from - offset;
    u8* out = static_cast<u8*>(buffer);
    for (u64 i = 0; i < n; ++i) out[i] = f.data[offset + i];
    return static_cast<i64>(n);
}

void fake_close(i64) { ++g_fake_closed; }

struct FakeEntry {
    const char* name;
    u64 size;
    u64 mtime;
    bool directory;
};

FakeEntry g_fake_entries[] = {
    { "a.txt", 11, 1000, false },
    { "b.dat", 300, 2000, false },
    { "sub", 0, 3000, true },
    { "C.TXT", 5, 4000, false },
};

i32 fake_dir(u64 index, char* name, u32 capacity, u64* size, u64* mtime, bool* directory) {
    if (index >= 4) return 0;
    const FakeEntry& e = g_fake_entries[index];
    u32 n = 0;
    while (e.name[n] && n + 1 < capacity) {
        name[n] = e.name[n];
        ++n;
    }
    name[n] = 0;
    *size = e.size;
    *mtime = e.mtime;
    *directory = e.directory;
    return 1;
}

pehost::Clock g_fake_clock{};
bool g_fake_clock_ok = true;

bool fake_clock(pehost::Clock* out) {
    *out = g_fake_clock;
    return g_fake_clock_ok;
}

int g_fls_hits = 0;
u64 g_fls_last = 0;

PEHOST_WINAPI void fls_callback(void* value) {
    ++g_fls_hits;
    g_fls_last = reinterpret_cast<u64>(value);
}

int g_locale_hits = 0;
u16 g_locale_first[16];

PEHOST_WINAPI i32 locale_callback(u16* name) {
    ++g_locale_hits;
    for (u32 i = 0; i < 15 && name[i]; ++i) g_locale_first[i] = name[i];
    return 1;
}

u16 wstr_buf[4][300];
const u16* wide(const char* text, u32 slot = 0) {
    u32 i = 0;
    for (; text[i]; ++i) wstr_buf[slot][i] = static_cast<u8>(text[i]);
    wstr_buf[slot][i] = 0;
    return wstr_buf[slot];
}

struct FakeTypeInfo {
    void* vtable;
    void* undecorated;
    char decorated[24];
};

const char* g_stdin_feed = nullptr;
u32 g_stdin_taken = 0;

i64 fake_stdin(void* buffer, u32 length) {
    if (!g_stdin_feed) return -1;
    u32 left = 0;
    while (g_stdin_feed[g_stdin_taken + left]) ++left;
    u32 n = length < left ? length : left;
    for (u32 i = 0; i < n; ++i) static_cast<char*>(buffer)[i] = g_stdin_feed[g_stdin_taken + i];
    g_stdin_taken += n;
    return n;
}

void extended_tests() {
    using namespace pehost::win32;
    using namespace pehost;
    static pehost::Teb teb;
    static pehost::Peb peb;
    teb.self = reinterpret_cast<u64>(&teb);
    teb.client_thread = 77;
    teb.client_process = 5;
    lx(SYS_ARCH_PRCTL, 0x1001, reinterpret_cast<u64>(&teb));
    for (u32 i = 0; i < 300; ++i) g_fake_pattern[i] = static_cast<u8>(i * 7 + 3);

    static pehost::Runtime ext = g_recording;
    ext.clock = &fake_clock;
    ext.file_open = &fake_open;
    ext.file_read = &fake_read;
    ext.file_close = &fake_close;
    ext.dir_entry = &fake_dir;
    ext.read_stdin = &fake_stdin;
    pehost::g_runtime = &ext;
    g_rec = Recorder{};
    pehost::World& w = pehost::g_world;
    w.heap.init(&test_map);
    w.imports = pehost::all_imports();
    g_extra = Extra{};
    for (Search& s : g_searches) s = Search{};

    g_fake_clock = pehost::Clock{ 12345, 10000000, 0, false };
    i64 value = 0;
    set_last_error(0);
    expect(QueryPerformanceFrequency(&value) == 1 && value == 10000000 && QueryPerformanceCounter(&value) == 1 && value == 12345 && last_error() == 0, "the performance counter reports the platform frequency and count");
    expect(QueryPerformanceFrequency(nullptr) == 0 && last_error() == pehost::ERROR_INVALID_PARAMETER && QueryPerformanceCounter(nullptr) == 0, "a null output is ERROR_INVALID_PARAMETER");
    g_fake_clock = pehost::Clock{ 5000000, 10000000, 0, false };
    expect(GetTickCount64() == 500, "ticks are the counter scaled to milliseconds");
    g_fake_clock = pehost::Clock{ 0x100000005ull, 1000, 0, false };
    expect(GetTickCount64() == 0x100000005ull && GetTickCount() == 5, "GetTickCount is the low 32 bits of GetTickCount64");
    g_fake_clock = pehost::Clock{ 0xFFFFFFFFFFFFFFull, 3000000000ull, 0, false };
    expect(GetTickCount64() == 0xFFFFFFFFFFFFFFull / 3000000000ull * 1000 + 0xFFFFFFFFFFFFFFull % 3000000000ull * 1000 / 3000000000ull, "scaling does not overflow with a large counter");
    u32 ft[2] = { 1, 1 };
    g_fake_clock = pehost::Clock{ 20000000, 10000000, 0, true };
    GetSystemTimeAsFileTime(ft);
    expect(((static_cast<u64>(ft[1]) << 32) | ft[0]) == 116444736000000000ull, "a valid wall clock at the Unix epoch is the FILETIME of 1970");
    g_fake_clock = pehost::Clock{ 20000000, 10000000, 86400000, true };
    GetSystemTimeAsFileTime(ft);
    expect(((static_cast<u64>(ft[1]) << 32) | ft[0]) == 116444736000000000ull + 864000000000ull, "one day later adds exactly 864e9 ticks");
    g_fake_clock = pehost::Clock{ 20000000, 10000000, 99, false };
    GetSystemTimeAsFileTime(ft);
    expect(((static_cast<u64>(ft[1]) << 32) | ft[0]) == 2000ull * 10000, "without a wall clock the uptime is used as the time");
    g_fake_clock_ok = false;
    set_last_error(0);
    expect(QueryPerformanceFrequency(&value) == 0 && last_error() == pehost::ERROR_NOT_SUPPORTED && QueryPerformanceCounter(&value) == 0 && GetTickCount64() == 0, "a clock that fails is reported as unsupported");
    g_fake_clock_ok = true;
    g_fake_clock = pehost::Clock{ 1, 0, 0, false };
    expect(QueryPerformanceFrequency(&value) == 0 && GetTickCount64() == 0, "a zero frequency is not accepted");
    ext.clock = nullptr;
    set_last_error(0);
    expect(QueryPerformanceCounter(&value) == 0 && last_error() == pehost::ERROR_NOT_SUPPORTED && GetTickCount() == 0, "without a clock hook the time functions fail cleanly");
    ext.clock = &fake_clock;

    expect(IsProcessorFeaturePresent(10) == 1 && IsProcessorFeaturePresent(25) == 0 && IsDebuggerPresent() == 0 && GetActiveProcessorCount(0xFFFF) == 1, "processor features, the debugger and the processor count");
    u8 startup[120];
    for (u8& b : startup) b = 0xAA;
    GetStartupInfoA(startup);
    bool tail_clear = true;
    for (u32 i = 4; i < 104; ++i) tail_clear = tail_clear && startup[i] == 0;
    expect(*reinterpret_cast<u32*>(startup) == 104 && tail_clear && startup[104] == 0xAA, "GetStartupInfo fills exactly 104 bytes with the size first");
    expect(EncodePointer(0x1234) == 0x1234 && DecodePointer(0x1234) == 0x1234, "pointer encoding is the identity");

    alignas(16) static u64 head[2];
    alignas(16) static u64 entries[3][2];
    InitializeSListHead(head);
    expect(InterlockedPopEntrySList(head) == 0 && QueryDepthSList(head) == 0, "an empty list pops nothing");
    u64 first = InterlockedPushEntrySList(head, entries[0]);
    u64 second = InterlockedPushEntrySList(head, entries[1]);
    InterlockedPushEntrySList(head, entries[2]);
    expect(first == 0 && second == reinterpret_cast<u64>(entries[0]) && QueryDepthSList(head) == 3, "pushes return the previous top and the depth counts");
    expect(InterlockedPopEntrySList(head) == reinterpret_cast<u64>(entries[2]) && QueryDepthSList(head) == 2, "pops come back last in first out");
    u64 flushed = InterlockedFlushSList(head);
    expect(flushed == reinterpret_cast<u64>(entries[1]) && QueryDepthSList(head) == 0 && InterlockedPopEntrySList(head) == 0, "flush returns the chain and empties the list");

    set_last_error(0);
    u32 f0 = FlsAlloc(&fls_callback);
    u32 f1 = FlsAlloc(nullptr);
    expect(f0 == 0 && f1 == 1 && FlsGetValue(f0) == nullptr && last_error() == 0, "FLS indexes are handed out in order and start empty");
    expect(FlsSetValue(f0, reinterpret_cast<void*>(0x77)) == 1 && FlsGetValue(f0) == reinterpret_cast<void*>(0x77), "a stored value reads back");
    g_fls_hits = 0;
    expect(FlsFree(f1) == 1 && g_fls_hits == 0, "freeing a slot without a callback is silent");
    expect(FlsFree(f0) == 1 && g_fls_hits == 1 && g_fls_last == 0x77, "freeing a slot runs its callback with the value");
    f0 = FlsAlloc(&fls_callback);
    FlsFree(f0);
    expect(g_fls_hits == 1, "a callback is not run for an empty value");
    expect(FlsFree(0) == 0 && last_error() == pehost::ERROR_INVALID_PARAMETER && FlsGetValue(0) == nullptr && FlsSetValue(0, nullptr) == 0 && FlsFree(200) == 0 && FlsGetValue(200) == nullptr && FlsSetValue(200, nullptr) == 0,
           "a freed or out of range index is invalid everywhere");
    bool all = true;
    for (u32 i = 0; i < MAX_FLS; ++i) all = all && FlsAlloc(nullptr) == i;
    expect(all && FlsAlloc(nullptr) == FLS_OUT_OF_INDEXES && last_error() == pehost::ERROR_NOT_ENOUGH_MEMORY, "128 FLS slots exist and then allocation fails");
    FlsFree(40);
    expect(FlsAlloc(nullptr) == 40, "the lowest freed slot is reused");

    u64 lock = 5;
    InitializeSRWLock(&lock);
    expect(lock == 0 && TryAcquireSRWLockExclusive(&lock) == 1 && TryAcquireSRWLockExclusive(&lock) == 0 && TryAcquireSRWLockShared(&lock) == 0, "an exclusive lock blocks everything else");
    ReleaseSRWLockExclusive(&lock);
    expect(TryAcquireSRWLockShared(&lock) == 1 && TryAcquireSRWLockShared(&lock) == 1 && TryAcquireSRWLockExclusive(&lock) == 0, "shared locks stack and block exclusive ones");
    ReleaseSRWLockShared(&lock);
    ReleaseSRWLockShared(&lock);
    ReleaseSRWLockShared(&lock);
    expect(lock == 0 && TryAcquireSRWLockExclusive(&lock) == 1, "releasing more than was taken does not corrupt the lock");
    u64 cond = 9;
    InitializeConditionVariable(&cond);
    WakeConditionVariable(&cond);
    WakeAllConditionVariable(&cond);
    expect(cond == 0, "condition variable wake calls are harmless without waiters");

    lock = 0;
    AcquireSRWLockExclusive(&lock);
    g_rec.sleeps = 0;
    set_last_error(0);
    expect(SleepConditionVariableSRW(&cond, &lock, 50, 0) == 0 && last_error() == pehost::ERROR_TIMEOUT && g_rec.sleeps == 1 && g_rec.slept == 50 && lock == 1, "a timed condition wait sleeps, times out and holds the lock again");
    ReleaseSRWLockExclusive(&lock);
    AcquireSRWLockShared(&lock);
    expect(SleepConditionVariableSRW(&cond, &lock, 10, 1) == 0 && lock == 2, "a shared wait gives the shared lock back");
    CriticalSection cs{};
    InitializeCriticalSection(&cs);
    EnterCriticalSection(&cs);
    expect(SleepConditionVariableCS(&cond, &cs, 7) == 0 && last_error() == pehost::ERROR_TIMEOUT && g_rec.slept == 7 && TryEnterCriticalSection(&cs) == 1, "a critical section wait leaves the section owned");
    expect(InitializeCriticalSectionEx(&cs, 0, 0) == 1 && InitializeCriticalSectionAndSpinCount(&cs, 4000) == 1 && SetCriticalSectionSpinCount(&cs, 1) == 0, "the extended critical section initialisers succeed");

    pehost::g_runtime = &g_child_runtime;
    ChildResult infinite = in_child([] {
        u64 l = 0, c = 0;
        AcquireSRWLockExclusive(&l);
        SleepConditionVariableSRW(&c, &l, 0xFFFFFFFFu, 0);
    });
    ChildResult contended = in_child([] {
        u64 l = 0;
        AcquireSRWLockExclusive(&l);
        AcquireSRWLockExclusive(&l);
    });
    ChildResult shared_blocked = in_child([] {
        u64 l = 1;
        AcquireSRWLockShared(&l);
    });
    ChildResult raised = in_child([] { RaiseException(0xE0001234u, 0, 0, nullptr); });
    ChildResult unwound = in_child([] { RtlUnwind(nullptr, nullptr, nullptr, nullptr); });
    ChildResult virtual_unwound = in_child([] { RtlVirtualUnwind(0, 0, 0, nullptr, nullptr, nullptr, nullptr, nullptr); });
    expect(infinite.exited && static_cast<u32>(infinite.code) == STATUS_POSSIBLE_DEADLOCK && contains(infinite.out, "no other thread can end"), "an infinite wait that nothing can end stops the process instead of hanging");
    expect(contended.exited && static_cast<u32>(contended.code) == STATUS_POSSIBLE_DEADLOCK && shared_blocked.exited && static_cast<u32>(shared_blocked.code) == STATUS_POSSIBLE_DEADLOCK,
           "taking a lock nobody can release stops the process");
    expect(raised.exited && static_cast<u32>(raised.code) == 0xE0001234u && contains(raised.out, "status 0xE0001234") && unwound.exited && static_cast<u32>(unwound.code) == STATUS_NOT_SUPPORTED &&
               virtual_unwound.exited && static_cast<u32>(virtual_unwound.code) == STATUS_NOT_SUPPORTED && contains(unwound.out, "unwinding is not supported"),
           "an exception that nothing can handle or unwind ends the process with its code and says why");
    pehost::g_runtime = &ext;

    u8 context[1232];
    for (u8& b : context) b = 0xCC;
    RtlCaptureContext(context);
    u64 base_out = 77;
    expect(*reinterpret_cast<u32*>(context + 0x30) == 0x10000F && context[0] == 0 && context[1231] == 0 && RtlLookupFunctionEntry(0x1000, &base_out, nullptr) == nullptr && base_out == 0 && UnhandledExceptionFilter(nullptr) == 0,
           "context capture fills a CONTEXT and function lookup finds no unwind data");
    alignas(4096) static u8 fake_image[8192];
    w.image = fake_image;
    w.image_pages = 2;
    u64 header = 5;
    expect(RtlPcToFileHeader(reinterpret_cast<u64>(fake_image) + 100, &header) == reinterpret_cast<u64>(fake_image) && header == reinterpret_cast<u64>(fake_image) &&
               RtlPcToFileHeader(reinterpret_cast<u64>(fake_image) + 8192, &header) == 0 && header == 0 && RtlPcToFileHeader(0, nullptr) == 0,
           "an address inside the image maps to its base and one past the end does not");
    u64 handle = 0;
    expect(GetModuleHandleExW(4, fake_image + 10, &handle) == 1 && handle == reinterpret_cast<u64>(fake_image) && GetModuleHandleExW(4, fake_image + 9000, &handle) == 0 && last_error() == pehost::ERROR_MOD_NOT_FOUND && handle == 0,
           "GetModuleHandleEx by address finds the image and fails outside it");
    expect(GetModuleHandleExW(0, wide("kernel32.dll"), &handle) == 1 && handle == GetModuleHandleA("kernel32.dll") && GetModuleHandleExW(1, wide("nosuch.dll"), &handle) == 0 && handle == 0 &&
               GetModuleHandleExW(3, wide("kernel32.dll"), &handle) == 0 && last_error() == pehost::ERROR_INVALID_PARAMETER && GetModuleHandleExW(0, wide("kernel32.dll"), nullptr) == 0 && GetModuleHandleExW(8, wide("kernel32.dll"), &handle) == 0,
           "GetModuleHandleEx by name works and rejects bad flags and a null output");

    detail::copy_text(w.module_path, MAX_MODULE_PATH, "C:\\dir\\prog.exe");
    u16 path[40];
    for (u16& c : path) c = 0xEEEE;
    expect(GetModuleFileNameW(0, path, 40) == 15 && wide_is(path, "C:\\dir\\prog.exe") && path[16] == 0xEEEE, "the wide module path is the narrow one converted");
    for (u16& c : path) c = 0xEEEE;
    set_last_error(0);
    expect(GetModuleFileNameW(0, path, 5) == 5 && path[3] == 'd' && path[4] == 0 && path[5] == 0xEEEE && last_error() == pehost::ERROR_INSUFFICIENT_BUFFER, "a short wide buffer gets a terminated truncation");
    expect(GetModuleFileNameW(0, path, 0) == 0 && last_error() == pehost::ERROR_INSUFFICIENT_BUFFER && GetModuleFileNameW(GetModuleHandleA("kernel32.dll"), path, 40) == 0, "a zero size and another module fail");
    set_last_error(0);
    u64 k32 = GetModuleHandleA("kernel32.dll");
    expect(LoadLibraryExA("kernel32.dll", 0, 0) == k32 && LoadLibraryExA("kernel32.dll", 0, 0x800) == k32 && LoadLibraryExW(wide("kernel32.dll"), 0, 0) == k32, "LoadLibraryEx without special flags is LoadLibrary");
    expect(LoadLibraryExA("kernel32.dll", 0, 0x2) == 0 && last_error() == pehost::ERROR_NOT_SUPPORTED && LoadLibraryExW(wide("kernel32.dll"), 0, 0x1) == 0 && LoadLibraryExA("kernel32.dll", 1, 0) == 0 && last_error() == pehost::ERROR_INVALID_PARAMETER,
           "data file and dependency flags are refused and a reserved handle is invalid");

    {
        char in[16] = {};
        u32 got = 99;
        g_stdin_feed = "hello";
        g_stdin_taken = 0;
        expect(ReadFile(HANDLE_STDIN, in, 3, &got, nullptr) == 1 && got == 3 && strncmp(in, "hel", 3) == 0, "standard input is read through the host");
        expect(ReadFile(HANDLE_STDIN, in, 16, &got, nullptr) == 1 && got == 2 && strncmp(in, "lo", 2) == 0, "a second read returns what is left");
        expect(ReadFile(HANDLE_STDIN, in, 16, &got, nullptr) == 1 && got == 0, "the end of standard input is success with zero bytes");
        got = 99;
        expect(ReadFile(HANDLE_STDIN, in, 0, &got, nullptr) == 1 && got == 0, "a zero-length read of standard input succeeds with nothing read");
        g_stdin_feed = nullptr;
        got = 99;
        expect(ReadFile(HANDLE_STDIN, in, 4, &got, nullptr) == 0 && got == 0 && last_error() == pehost::ERROR_BROKEN_PIPE, "a host that reports an error on standard input fails the read as a broken pipe");
        pehost::Runtime none = ext;
        none.read_stdin = nullptr;
        pehost::g_runtime = &none;
        got = 99;
        expect(ReadFile(HANDLE_STDIN, in, 4, &got, nullptr) == 1 && got == 0, "a host with no standard input reads as already at the end");
        pehost::g_runtime = &ext;
    }
    expect(GetFileType(HANDLE_STDOUT) == 3 && GetFileType(HANDLE_STDIN) == 3 && GetFileType(0x100) == 0 && last_error() == pehost::ERROR_INVALID_HANDLE, "standard handles are character devices and unknown handles are not files");
    expect(GetConsoleMode(HANDLE_STDOUT, nullptr) == 0 && last_error() == pehost::ERROR_INVALID_HANDLE && SetConsoleMode(HANDLE_STDOUT, 0) == 0 && GetConsoleScreenBufferInfo(HANDLE_STDOUT, nullptr) == 0 && ReadConsoleW(HANDLE_STDIN, nullptr, 0, nullptr, nullptr) == 0,
           "console queries fail as they do for a redirected handle");
    expect(GetConsoleOutputCP() == 65001 && GetConsoleCP() == 65001 && GetOEMCP() == 65001 && SetConsoleCtrlHandler(nullptr, 1) == 0 && last_error() == pehost::ERROR_NOT_SUPPORTED && SetStdHandle(0xFFFFFFF5u, 1) == 0, "code pages are UTF-8 and handler and handle changes are refused");

    u32 written = 99;
    g_rec = Recorder{};
    expect(WriteConsoleW(HANDLE_STDOUT, wide("hi"), 2, &written, nullptr) == 1 && written == 2 && same(g_rec.last_text, "hi") && g_rec.last_stream == 1 && WriteConsoleW(HANDLE_STDERR, wide("e"), 1, &written, nullptr) == 1 && g_rec.last_stream == 2,
           "WriteConsoleW writes UTF-8 to the right stream");
    expect(WriteConsoleW(0x100, wide("x"), 1, &written, nullptr) == 0 && written == 0 && last_error() == pehost::ERROR_INVALID_HANDLE && WriteConsoleW(HANDLE_STDOUT, wide("x"), 1, &written, &written) == 0 && last_error() == pehost::ERROR_INVALID_PARAMETER,
           "a bad handle or a reserved argument fails");
    u16 pair[129];
    for (u32 i = 0; i < 127; ++i) pair[i] = 'a';
    pair[127] = 0xD83D;
    pair[128] = 0xDE00;
    g_rec = Recorder{};
    expect(WriteConsoleW(HANDLE_STDOUT, pair, 129, &written, nullptr) == 1 && written == 129 && g_rec.writes == 2 && g_rec.last_length == 4 && static_cast<u8>(g_rec.last_text[0]) == 0xF0 && static_cast<u8>(g_rec.last_text[3]) == 0x80,
           "a surrogate pair is never split across the chunk boundary");

    expect(FlushFileBuffers(HANDLE_STDOUT) == 1 && FlushFileBuffers(0x100) == 0 && last_error() == pehost::ERROR_INVALID_HANDLE && CloseHandle(HANDLE_STDOUT) == 1 && CloseHandle(0x100) == 0, "flushing and closing the standard handles works and unknown handles fail");
    expect(CreateFileA("CONOUT$", 0xC0000000u, 0, nullptr, 3, 0, 0) == HANDLE_STDOUT && CreateFileA("CONIN$", 0x80000000u, 0, nullptr, 3, 0, 0) == HANDLE_STDIN, "the console device names open the standard handles");

    g_fake_opened = g_fake_closed = 0;
    u64 fh = CreateFileA("a.txt", 0x80000000u, 1, nullptr, 3, 0x80, 0);
    expect(fh == 0x100 && GetFileType(fh) == 1 && FlushFileBuffers(fh) == 1 && g_fake_opened == 1, "a read only open of an existing file gives a file handle");
    i64 size = 0;
    char buf[64];
    u32 got = 99;
    expect(GetFileSizeEx(fh, &size) == 1 && size == 11 && ReadFile(fh, buf, 5, &got, nullptr) == 1 && got == 5 && strncmp(buf, "hello", 5) == 0, "size and the first read");
    expect(ReadFile(fh, buf, 64, &got, nullptr) == 1 && got == 6 && strncmp(buf, " world", 6) == 0 && ReadFile(fh, buf, 64, &got, nullptr) == 1 && got == 0, "reading to the end returns the rest and then zero bytes with success");
    i64 pos = -1;
    expect(SetFilePointerEx(fh, 6, &pos, 0) == 1 && pos == 6 && ReadFile(fh, buf, 5, &got, nullptr) == 1 && strncmp(buf, "world", 5) == 0, "seeking from the start");
    expect(SetFilePointerEx(fh, -3, &pos, 1) == 1 && pos == 8 && SetFilePointerEx(fh, -5, &pos, 2) == 1 && pos == 6 && SetFilePointerEx(fh, 0, nullptr, 2) == 1, "seeking from the current position and from the end");
    set_last_error(0);
    expect(SetFilePointerEx(fh, -100, &pos, 1) == 0 && last_error() == pehost::ERROR_NEGATIVE_SEEK && SetFilePointerEx(fh, 0, &pos, 3) == 0 && last_error() == pehost::ERROR_INVALID_PARAMETER && SetFilePointerEx(fh, 0, &pos, 1) == 1 && pos == 11,
           "a seek before the start or with a bad method fails and keeps the position");
    expect(SetFilePointerEx(fh, 500, &pos, 0) == 1 && ReadFile(fh, buf, 4, &got, nullptr) == 1 && got == 0 && SetFilePointerEx(fh, 0, &pos, 1) == 1 && pos == 500, "seeking past the end is allowed and reads nothing there");
    expect(ReadFile(fh, buf, 4, &got, &got) == 0 && last_error() == pehost::ERROR_NOT_SUPPORTED && got == 0 && SetEndOfFile(fh) == 0 && last_error() == pehost::ERROR_ACCESS_DENIED && SetEndOfFile(0x104) == 0 && last_error() == pehost::ERROR_INVALID_HANDLE,
           "overlapped reads and truncation are refused");
    g_stdin_feed = "";
    g_stdin_taken = 0;
    expect(ReadFile(HANDLE_STDIN, buf, 4, &got, nullptr) == 1 && got == 0 && ReadFile(0x104, buf, 4, &got, nullptr) == 0 && last_error() == pehost::ERROR_INVALID_HANDLE && GetFileSizeEx(0x104, &size) == 0 && SetFilePointerEx(0x104, 0, &pos, 0) == 0,
           "standard input reads end of file and unknown handles fail");
    expect(CloseHandle(fh) == 1 && g_fake_closed == 1 && CloseHandle(fh) == 0 && last_error() == pehost::ERROR_INVALID_HANDLE && ReadFile(fh, buf, 1, &got, nullptr) == 0, "closing releases the platform file and a second close fails");
    expect(CreateFileA("a.txt", 0x80000000u, 1, nullptr, 3, 0, 0) == 0x100 && CloseHandle(0x102) == 0, "the lowest handle is reused and a misaligned handle is invalid");
    CloseHandle(0x100);

    set_last_error(0);
    expect(CreateFileA("missing.txt", 0x80000000u, 1, nullptr, 3, 0, 0) == INVALID_HANDLE && last_error() == pehost::ERROR_FILE_NOT_FOUND && CreateFileA("missing.txt", 0x80000000u, 1, nullptr, 4, 0, 0) == INVALID_HANDLE && last_error() == pehost::ERROR_ACCESS_DENIED,
           "a missing file is ERROR_FILE_NOT_FOUND, and creating one is denied");
    expect(CreateFileA("dir\\a.txt", 0x80000000u, 1, nullptr, 3, 0, 0) == INVALID_HANDLE && last_error() == pehost::ERROR_PATH_NOT_FOUND && CreateFileA("..", 0x80000000u, 1, nullptr, 3, 0, 0) == INVALID_HANDLE && CreateFileA(nullptr, 0, 0, nullptr, 3, 0, 0) == INVALID_HANDLE && last_error() == pehost::ERROR_INVALID_PARAMETER,
           "a path with a separator is not found and a null name is invalid");
    expect(CreateFileA("a.txt", 0x40000000u, 1, nullptr, 3, 0, 0) == INVALID_HANDLE && last_error() == pehost::ERROR_ACCESS_DENIED && CreateFileA("a.txt", 0x80000000u, 1, nullptr, 2, 0, 0) == INVALID_HANDLE && last_error() == pehost::ERROR_ACCESS_DENIED &&
               CreateFileA("a.txt", 0x80000000u, 1, nullptr, 3, 0x40000000u, 0) == INVALID_HANDLE && last_error() == pehost::ERROR_NOT_SUPPORTED && g_fake_opened == 2,
           "write access, creating dispositions and overlapped files are refused before the file is touched");
    u64 bh = CreateFileW(wide("b.dat"), 0x80000000u, 1, nullptr, 3, 0, 0);
    expect(bh == 0x100 && CreateFileW(wide("nope"), 0x80000000u, 1, nullptr, 3, 0, 0) == INVALID_HANDLE && last_error() == pehost::ERROR_FILE_NOT_FOUND && CreateFileW(nullptr, 0, 0, nullptr, 3, 0, 0) == INVALID_HANDLE && last_error() == pehost::ERROR_INVALID_PARAMETER,
           "the wide open is the narrow open");
    g_fake_chunk = 7;
    u8 big[300];
    got = 0;
    expect(ReadFile(bh, big, 300, &got, nullptr) == 1 && got == 300 && g_fake_last_offset == 294, "a platform that returns short reads is asked again until the request is filled");
    bool intact = true;
    for (u32 i = 0; i < 300; ++i) intact = intact && big[i] == g_fake_pattern[i];
    expect(intact, "and the data arrives intact and in order");
    g_fake_chunk = 0;
    SetFilePointerEx(bh, 0, nullptr, 0);
    g_fake_fail_from = 100;
    set_last_error(0);
    expect(ReadFile(bh, big, 300, &got, nullptr) == 0 && got == 100 && last_error() == pehost::ERROR_INVALID_FUNCTION && SetFilePointerEx(bh, 0, &pos, 1) == 1 && pos == 100, "a read error part way reports the bytes read and fails");
    g_fake_fail_from = ~0ull;
    CloseHandle(bh);

    u64 many[MAX_OPEN_FILES];
    bool filled = true;
    for (u32 i = 0; i < MAX_OPEN_FILES; ++i) {
        many[i] = CreateFileA("a.txt", 0x80000000u, 1, nullptr, 3, 0, 0);
        filled = filled && many[i] == 0x100 + 4ull * i;
    }
    g_fake_closed = 0;
    expect(filled && CreateFileA("a.txt", 0x80000000u, 1, nullptr, 3, 0, 0) == INVALID_HANDLE && last_error() == pehost::ERROR_NOT_ENOUGH_MEMORY && g_fake_closed == 1, "32 files can be open and the next open fails after releasing its platform file");
    CloseHandle(many[5]);
    expect(CreateFileA("a.txt", 0x80000000u, 1, nullptr, 3, 0, 0) == many[5], "a closed slot is reused");
    for (u32 i = 0; i < MAX_OPEN_FILES; ++i) CloseHandle(0x100 + 4ull * i);

    g_fake_closed = 0;
    set_last_error(0);
    expect(GetFileAttributesA("a.txt") == 0x80 && g_fake_closed == 1 && GetFileAttributesA("nope") == INVALID_FILE_ATTRIBUTES && last_error() == pehost::ERROR_FILE_NOT_FOUND && GetFileAttributesA("x\\y") == INVALID_FILE_ATTRIBUTES && last_error() == pehost::ERROR_PATH_NOT_FOUND &&
               GetFileAttributesA(nullptr) == INVALID_FILE_ATTRIBUTES && last_error() == pehost::ERROR_INVALID_PARAMETER && GetFileAttributesW(wide("a.txt")) == 0x80 && GetFileAttributesW(nullptr) == INVALID_FILE_ATTRIBUTES,
           "file attributes open and release the file and report the right errors");
    u8 attr[40];
    for (u8& b : attr) b = 0xAA;
    expect(GetFileAttributesExA("b.dat", 0, attr) == 1 && *reinterpret_cast<u32*>(attr) == 0x80 && *reinterpret_cast<u32*>(attr + 28) == 0 && *reinterpret_cast<u32*>(attr + 32) == 300 && attr[36] == 0xAA && attr[4] == 0,
           "extended attributes carry the size in the right fields and clear the times");
    expect(GetFileAttributesExW(wide("b.dat"), 0, attr) == 1 && GetFileAttributesExA("b.dat", 1, attr) == 0 && last_error() == pehost::ERROR_INVALID_PARAMETER && GetFileAttributesExA("nope", 0, attr) == 0 && last_error() == pehost::ERROR_FILE_NOT_FOUND && GetFileAttributesExA("a.txt", 0, nullptr) == 0,
           "the wide form works and a bad level, a missing file and a null buffer fail");
    expect(DeleteFileA("a.txt") == 0 && last_error() == pehost::ERROR_ACCESS_DENIED && DeleteFileW(wide("a.txt")) == 0 && RemoveDirectoryA("x") == 0 && CreateDirectoryW(wide("x"), nullptr) == 0 && MoveFileExW(wide("a", 0), wide("b", 1), 0) == 0 && SetFileAttributesA("a.txt", 1) == 0,
           "everything that would change the file system is denied");

    char cwd[8];
    u16 wcwd[8];
    expect(GetCurrentDirectoryA(8, cwd) == 1 && same(cwd, "\\") && GetCurrentDirectoryA(1, cwd) == 2 && GetCurrentDirectoryW(8, wcwd) == 1 && wide_is(wcwd, "\\") && GetCurrentDirectoryW(0, wcwd) == 2, "the current directory is the root and a short buffer gets the required size");
    expect(SetCurrentDirectoryW(wide("\\")) == 1 && SetCurrentDirectoryW(wide("C:\\x")) == 0 && last_error() == pehost::ERROR_FILE_NOT_FOUND && SetCurrentDirectoryW(nullptr) == 0, "only the root can be made current");
    char full[40];
    char* part = nullptr;
    expect(GetFullPathNameA("foo.txt", 40, full, &part) == 8 && same(full, "\\foo.txt") && part == full + 1 && GetFullPathNameA("C:\\x\\y.z", 40, full, &part) == 8 && same(full, "C:\\x\\y.z") && part == full + 5 && GetFullPathNameA("/a/b", 40, full, nullptr) == 4,
           "relative names are rooted, absolute names kept and the file part is located");
    expect(GetFullPathNameA("foo.txt", 4, full, &part) == 9 && GetFullPathNameA("", 40, full, &part) == 0 && last_error() == pehost::ERROR_INVALID_PARAMETER && GetFullPathNameA(nullptr, 40, full, &part) == 0, "a short buffer gets the required size and an empty name is invalid");
    u16 wfull[40];
    u16* wpart = nullptr;
    expect(GetFullPathNameW(wide("dir\\foo.txt"), 40, wfull, &wpart) == 12 && wide_is(wfull, "\\dir\\foo.txt") && wide_is(wpart, "foo.txt") && GetFullPathNameW(wide("foo"), 3, wfull, &wpart) == 5 && GetFullPathNameW(nullptr, 40, wfull, &wpart) == 0, "the wide full path matches");
    char temp[8];
    expect(GetTempPathA(8, temp) == 1 && same(temp, "\\") && GetTempPathA(1, temp) == 2, "the temporary directory is the root");

    void* block = HeapAlloc(GetProcessHeap(), 0, 32);
    expect(LocalFree(0) == 0 && LocalFree(reinterpret_cast<u64>(block)) == 0 && LocalFree(0x1234) == 0x1234 && last_error() == pehost::ERROR_INVALID_HANDLE, "LocalFree releases heap memory and returns the argument for a foreign pointer");

    char ebuf[300];
    set_last_error(0);
    expect(GetEnvironmentVariableA("ZED", ebuf, 300) == 0 && last_error() == pehost::ERROR_ENVVAR_NOT_FOUND && SetEnvironmentVariableA("Zed", "one") == 1 && GetEnvironmentVariableA("ZED", ebuf, 300) == 3 && same(ebuf, "one") && GetEnvironmentVariableA("zed", ebuf, 3) == 4,
           "a variable is set, found regardless of case, and a short buffer gets the size with the terminator");
    expect(SetEnvironmentVariableA("ZED", "two!") == 1 && GetEnvironmentVariableA("Zed", ebuf, 300) == 4 && same(ebuf, "two!") && GetEnvironmentVariableA("Zed", ebuf, 5) == 4, "setting again replaces the value and an exact fit succeeds");
    expect(SetEnvironmentVariableW(wide("W", 0), wide("wide", 1)) == 1 && GetEnvironmentVariableA("w", ebuf, 300) == 4 && same(ebuf, "wide"), "wide variables are the same store");
    u16 webuf[16];
    expect(GetEnvironmentVariableW(wide("W"), webuf, 16) == 4 && wide_is(webuf, "wide") && GetEnvironmentVariableW(wide("W"), webuf, 4) == 5 && GetEnvironmentVariableW(wide("NOPE"), webuf, 16) == 0 && last_error() == pehost::ERROR_ENVVAR_NOT_FOUND,
           "wide lookups, short buffers and misses");
    u16* strings = GetEnvironmentStringsW();
    u32 total = 0;
    while (strings[total] || strings[total + 1]) ++total;
    expect(strings != nullptr && wide_is(strings, "Zed=two!") && wide_is(strings + 9, "W=wide") && total == 9 + 6 && FreeEnvironmentStringsW(strings) == 1 && FreeEnvironmentStringsW(nullptr) == 0, "the environment block lists NAME=value entries and ends with an extra zero");
    expect(SetEnvironmentVariableA("ZED", nullptr) == 1 && GetEnvironmentVariableA("Zed", ebuf, 300) == 0 && SetEnvironmentVariableA("ZED", nullptr) == 0 && last_error() == pehost::ERROR_ENVVAR_NOT_FOUND && SetEnvironmentVariableW(wide("W"), nullptr) == 1, "a null value removes the variable and removing it twice fails");
    char longv[300];
    for (u32 i = 0; i < 299; ++i) longv[i] = 'v';
    longv[299] = 0;
    longv[255] = 0;
    expect(SetEnvironmentVariableA("A=B", "x") == 0 && last_error() == pehost::ERROR_INVALID_PARAMETER && SetEnvironmentVariableA("", "x") == 0 && SetEnvironmentVariableA(nullptr, "x") == 0 && SetEnvironmentVariableA("L", longv) == 1, "bad names are rejected and a 255 character value fits");
    longv[255] = 'v';
    longv[256] = 0;
    expect(SetEnvironmentVariableA("L2", longv) == 0 && last_error() == pehost::ERROR_INVALID_PARAMETER, "a 256 character value does not fit");
    g_extra = Extra{};
    bool env_full = true;
    char nm[8];
    for (u32 i = 0; i < MAX_ENV; ++i) {
        nm[0] = 'N';
        nm[1] = static_cast<char>('a' + i / 10);
        nm[2] = static_cast<char>('a' + i % 10);
        nm[3] = 0;
        env_full = env_full && SetEnvironmentVariableA(nm, "v") == 1;
    }
    expect(env_full && SetEnvironmentVariableA("OneTooMany", "v") == 0 && last_error() == pehost::ERROR_NOT_ENOUGH_MEMORY, "the table holds 32 variables and the next one fails");
    g_extra = Extra{};

    expect(GetUserDefaultLCID() == 0x409 && GetSystemDefaultLCID() == 0x409 && IsValidLocale(0x409, 1) == 1 && IsValidLocale(0x411, 1) == 0 && IsValidCodePage(65001) == 1 && IsValidCodePage(1252) == 0, "locale and code page validity");
    u8 cpinfo[20];
    for (u8& b : cpinfo) b = 0xAA;
    expect(GetCPInfo(65001, cpinfo) == 1 && *reinterpret_cast<u32*>(cpinfo) == 4 && cpinfo[4] == '?' && cpinfo[5] == 0 && cpinfo[19] == 0 && GetCPInfo(1252, cpinfo) == 0 && last_error() == pehost::ERROR_INVALID_PARAMETER && GetCPInfo(65001, nullptr) == 0, "GetCPInfo describes UTF-8");
    g_locale_hits = 0;
    expect(EnumSystemLocalesW(&locale_callback, 1) == 1 && g_locale_hits == 1 && wide_is(g_locale_first, "00000409") && EnumSystemLocalesW(nullptr, 1) == 0, "locale enumeration lists en-US once");
    u16 li[16];
    expect(GetLocaleInfoW(0x409, 0x0E, li, 16) == 2 && wide_is(li, ".") && GetLocaleInfoW(0x409, 0x0F, li, 16) == 2 && wide_is(li, ",") && GetLocaleInfoW(0x409, 0x5C, li, 16) == 6 && wide_is(li, "en-US") && GetLocaleInfoW(0x409, 0x0E, nullptr, 0) == 2,
           "locale strings and the size query");
    set_last_error(0);
    expect(GetLocaleInfoW(0x409, 0x5C, li, 3) == 0 && last_error() == pehost::ERROR_INSUFFICIENT_BUFFER && GetLocaleInfoW(0x409, 0x7777, li, 16) == 0 && last_error() == pehost::ERROR_INVALID_FLAGS && GetLocaleInfoW(0x411, 0x0E, li, 16) == 0 && last_error() == pehost::ERROR_INVALID_PARAMETER,
           "a short buffer, an unknown type and an unknown locale fail with the matching errors");
    expect(GetLocaleInfoW(0x409, 0x20001004u, li, 2) == 2 && *reinterpret_cast<u32*>(li) == 65001 && GetLocaleInfoW(0x409, 0x20000001u, li, 2) == 2 && *reinterpret_cast<u32*>(li) == 0x409 && GetLocaleInfoW(0x409, 0x20007777u, li, 2) == 0 && GetLocaleInfoW(0x409, 0x20001004u, li, 1) == 0,
           "number valued locale queries return a DWORD");
    expect(GetUserDefaultLocaleName(li, 16) == 6 && wide_is(li, "en-US") && GetUserDefaultLocaleName(li, 3) == 0, "the default locale name is en-US");

    expect(CompareStringW(0x409, 0, wide("abc", 0), -1, wide("abd", 1), -1) == 1 && CompareStringW(0x409, 0, wide("abc", 0), 3, wide("abc", 1), 3) == 2 && CompareStringW(0x409, 0, wide("abd", 0), -1, wide("abc", 1), -1) == 3, "string comparison is less, equal and greater");
    expect(CompareStringW(0x409, 0, wide("ABC", 0), -1, wide("abc", 1), -1) != 2 && CompareStringW(0x409, 1, wide("ABC", 0), -1, wide("abc", 1), -1) == 2 && CompareStringW(0x409, 0, wide("ab", 0), -1, wide("abc", 1), -1) == 1 && CompareStringW(0x409, 0, wide("abc", 0), 2, wide("abc", 1), 3) == 1,
           "comparison honours the ignore case flag and lengths");
    set_last_error(0);
    expect(CompareStringW(0x409, 0x8, wide("a", 0), 1, wide("a", 1), 1) == 0 && last_error() == pehost::ERROR_INVALID_FLAGS && CompareStringEx(nullptr, 0, nullptr, 1, wide("a"), 1, nullptr, nullptr, 0) == 0 && last_error() == pehost::ERROR_INVALID_PARAMETER &&
               CompareStringEx(nullptr, 0x1, wide("Q", 0), 1, wide("q", 1), 1, nullptr, nullptr, 0) == 2, "unsupported flags and null strings fail and the Ex form compares");
    u16 mapped[16];
    expect(LCMapStringW(0x409, 0x200, wide("aBc"), -1, mapped, 16) == 4 && wide_is(mapped, "ABC") && LCMapStringEx(nullptr, 0x100, wide("aBc"), 3, mapped, 16, nullptr, nullptr, 0) == 3 && mapped[0] == 'a' && mapped[2] == 'c' && LCMapStringW(0x409, 0x200, wide("aBc"), 3, nullptr, 0) == 3,
           "case mapping, the terminator with a length of -1 and the size query");
    expect(LCMapStringW(0x409, 0x200, wide("abc"), 3, mapped, 2) == 0 && last_error() == pehost::ERROR_INSUFFICIENT_BUFFER && LCMapStringW(0x409, 0x400, wide("a"), 1, mapped, 16) == 0 && last_error() == pehost::ERROR_NOT_SUPPORTED && LCMapStringW(0x409, 0, wide("a"), 1, mapped, 16) == 0 && last_error() == pehost::ERROR_INVALID_FLAGS && LCMapStringW(0x409, 0x200, wide("a"), 0, mapped, 16) == 0,
           "a short buffer, an unsupported mapping and missing flags fail");
    const u16 types_in[] = { 'A', 'f', '5', ' ', '\t', '!', 1, 0xE9 };
    u16 types[8];
    expect(GetStringTypeW(1, types_in, 8, types) == 1 && types[0] == 0x181 && types[1] == 0x182 && types[2] == 0x84 && types[3] == 0x48 && types[4] == 0x68 && types[5] == 0x10 && types[6] == 0x20 && types[7] == 0 && GetStringTypeW(2, types_in, 8, types) == 0 && GetStringTypeW(1, types_in, 0, types) == 0,
           "character classification of letters, digits, blanks, punctuation and controls");

    u8 fd[600];
    set_last_error(0);
    u64 s1 = FindFirstFileExA("*.txt", 0, fd, 0, nullptr, 0);
    u64 stamp = (1000ull + 11644473600000ull) * 10000;
    u64 write_time = static_cast<u64>(*reinterpret_cast<u32*>(fd + 24)) << 32 | *reinterpret_cast<u32*>(fd + 20);
    expect(s1 == 0x1000 && *reinterpret_cast<u32*>(fd) == 0x80 && *reinterpret_cast<u32*>(fd + 28) == 0 && *reinterpret_cast<u32*>(fd + 32) == 11 && same(reinterpret_cast<char*>(fd + 44), "a.txt") && write_time == stamp,
           "the first match of a pattern fills attributes, times, size and name");
    expect(FindNextFileA(s1, fd) == 1 && same(reinterpret_cast<char*>(fd + 44), "C.TXT") && *reinterpret_cast<u32*>(fd + 32) == 5 && FindNextFileA(s1, fd) == 0 && last_error() == pehost::ERROR_NO_MORE_FILES,
           "patterns match without regard to case and the end is ERROR_NO_MORE_FILES");
    expect(FindClose(s1) == 1 && FindClose(s1) == 0 && last_error() == pehost::ERROR_INVALID_HANDLE && FindNextFileA(s1, fd) == 0 && last_error() == pehost::ERROR_INVALID_HANDLE, "a closed search is invalid");
    u64 s2 = FindFirstFileExA("*", 1, fd, 0, nullptr, 0);
    u32 listed = 1;
    bool saw_dir = *reinterpret_cast<u32*>(fd) == 0x80 && same(reinterpret_cast<char*>(fd + 44), "a.txt");
    while (FindNextFileA(s2, fd)) {
        ++listed;
        if (same(reinterpret_cast<char*>(fd + 44), "sub")) saw_dir = saw_dir && *reinterpret_cast<u32*>(fd) == 0x10;
    }
    FindClose(s2);
    expect(s2 == 0x1000 && listed == 4 && saw_dir, "a bare star lists everything and directories carry the directory attribute");
    u32 star_dot_star = 0, question = 0;
    u64 s3 = FindFirstFileExA("*.*", 0, fd, 0, nullptr, 0);
    do ++star_dot_star;
    while (FindNextFileA(s3, fd));
    FindClose(s3);
    u64 s4 = FindFirstFileExA("?.txt", 0, fd, 0, nullptr, 0);
    do ++question;
    while (FindNextFileA(s4, fd));
    FindClose(s4);
    expect(star_dot_star == 4 && question == 2, "star dot star matches names without a dot and a question mark matches one character");
    set_last_error(0);
    expect(FindFirstFileExA("nomatch*", 0, fd, 0, nullptr, 0) == INVALID_HANDLE && last_error() == pehost::ERROR_FILE_NOT_FOUND && FindFirstFileExA("sub\\x", 0, fd, 0, nullptr, 0) == INVALID_HANDLE && last_error() == pehost::ERROR_PATH_NOT_FOUND &&
               FindFirstFileExA("", 0, fd, 0, nullptr, 0) == INVALID_HANDLE && last_error() == pehost::ERROR_PATH_NOT_FOUND && FindFirstFileExA(nullptr, 0, fd, 0, nullptr, 0) == INVALID_HANDLE,
           "no match is file not found, and a pattern that names a directory is path not found");
    u64 s5 = FindFirstFileExA(".\\b*", 0, fd, 0, nullptr, 0);
    u64 s6 = FindFirstFileExA("\\s*", 0, fd, 0, nullptr, 0);
    expect(s5 == 0x1000 && s6 == 0x1004 && same(reinterpret_cast<char*>(fd + 44), "sub") && FindClose(s5) == 1 && FindClose(s6) == 1, "a leading dot slash or slash is accepted and handles are small distinct numbers");
    expect(FindFirstFileExA("*", 2, fd, 0, nullptr, 0) == INVALID_HANDLE && last_error() == pehost::ERROR_INVALID_PARAMETER && FindFirstFileExA("*", 0, fd, 1, nullptr, 0) == INVALID_HANDLE && last_error() == pehost::ERROR_NOT_SUPPORTED &&
               FindFirstFileExA("*", 0, fd, 3, nullptr, 0) == INVALID_HANDLE && last_error() == pehost::ERROR_INVALID_PARAMETER && FindFirstFileExA("*", 0, nullptr, 0, nullptr, 0) == INVALID_HANDLE && last_error() == pehost::ERROR_INVALID_PARAMETER,
           "an unknown level, directory only searches, an unknown operation and a null buffer are refused");
    u64 plain = FindFirstFileA("b*", fd);
    u8 wide_data[600];
    u64 plain_wide = FindFirstFileW(wide("B*"), wide_data);
    expect(plain != INVALID_HANDLE && same(reinterpret_cast<char*>(fd + 44), "b.dat") && FindNextFileA(plain, fd) == 0 && FindClose(plain) == 1 && plain_wide != INVALID_HANDLE && *reinterpret_cast<u16*>(wide_data + 44) == 'b' && FindNextFileW(plain_wide, wide_data) == 0 && FindClose(plain_wide) == 1 &&
               FindFirstFileA("zzz", fd) == INVALID_HANDLE && last_error() == pehost::ERROR_FILE_NOT_FOUND && FindFirstFileW(wide("zzz"), wide_data) == INVALID_HANDLE,
           "FindFirstFile is the level zero search in its narrow and wide forms");
    u8 wfd[600];
    u64 w1 = FindFirstFileExW(wide("*.TXT"), 1, wfd, 0, nullptr, 0);
    expect(w1 != INVALID_HANDLE && *reinterpret_cast<u16*>(wfd + 44) == 'a' && *reinterpret_cast<u16*>(wfd + 48) == 't' && *reinterpret_cast<u16*>(wfd + 54) == 0 && FindNextFileA(w1, fd) == 0 && last_error() == pehost::ERROR_INVALID_HANDLE &&
               FindNextFileW(w1, wfd) == 1 && *reinterpret_cast<u16*>(wfd + 44) == 'C' && FindNextFileW(w1, wfd) == 0 && last_error() == pehost::ERROR_NO_MORE_FILES && FindClose(w1) == 1,
           "wide searches fill wide names, are not interchangeable with narrow ones and end the same way");
    expect(FindFirstFileExW(nullptr, 0, wfd, 0, nullptr, 0) == INVALID_HANDLE && last_error() == pehost::ERROR_PATH_NOT_FOUND && FindFirstFileExW(wide("*"), 0, nullptr, 0, nullptr, 0) == INVALID_HANDLE && last_error() == pehost::ERROR_INVALID_PARAMETER &&
               FindFirstFileExW(wide("*"), 0, wfd, 1, nullptr, 0) == INVALID_HANDLE && last_error() == pehost::ERROR_NOT_SUPPORTED, "wide error cases");
    u64 hs[MAX_SEARCHES];
    bool eight = true;
    for (u32 i = 0; i < MAX_SEARCHES; ++i) {
        hs[i] = FindFirstFileExA("*", 0, fd, 0, nullptr, 0);
        eight = eight && hs[i] == 0x1000 + 4ull * i;
    }
    expect(eight && FindFirstFileExA("*", 0, fd, 0, nullptr, 0) == INVALID_HANDLE && last_error() == pehost::ERROR_NOT_ENOUGH_MEMORY && FindClose(0x1002) == 0 && FindClose(0x2000) == 0, "eight searches can be open, the ninth fails and bad handles are rejected");
    for (u64 h : hs) FindClose(h);
    ext.dir_entry = nullptr;
    expect(FindFirstFileExA("*", 0, fd, 0, nullptr, 0) == INVALID_HANDLE && last_error() == pehost::ERROR_FILE_NOT_FOUND, "without a directory hook nothing is found");
    ext.dir_entry = &fake_dir;

    expect(VerSetConditionMask(0, 2, 3) == 0x18 && VerSetConditionMask(0, 3, 2) == 0x12 && VerSetConditionMask(VerSetConditionMask(0, 2, 3), 1, 3) == 0x1B && VerSetConditionMask(0, 0x80, 1) == (1ull << 21), "condition masks pack three bits per field");
    alignas(8) u8 vi[156];
    auto fill = [&](u32 major, u32 minor, u32 build, u32 platform, u16 spmaj, u16 spmin, u16 suite, u8 product) {
        for (u8& b : vi) b = 0;
        *reinterpret_cast<u32*>(vi) = 156;
        *reinterpret_cast<u32*>(vi + 4) = major;
        *reinterpret_cast<u32*>(vi + 8) = minor;
        *reinterpret_cast<u32*>(vi + 12) = build;
        *reinterpret_cast<u32*>(vi + 16) = platform;
        *reinterpret_cast<u16*>(vi + 148) = spmaj;
        *reinterpret_cast<u16*>(vi + 150) = spmin;
        *reinterpret_cast<u16*>(vi + 152) = suite;
        vi[154] = product;
    };
    auto both = [&](u32 condition) { return VerSetConditionMask(VerSetConditionMask(0, 2, static_cast<u8>(condition)), 1, static_cast<u8>(condition)); };
    fill(10, 0, 0, 0, 0, 0, 0, 0);
    set_last_error(0);
    expect(VerifyVersionInfoA(vi, 3, both(3)) == 1 && last_error() == 0, "Windows 10.0 satisfies at least 10.0");
    fill(6, 1, 0, 0, 0, 0, 0, 0);
    expect(VerifyVersionInfoA(vi, 3, both(3)) == 1, "and at least 6.1: a higher major version decides without looking at the minor");
    fill(11, 0, 0, 0, 0, 0, 0, 0);
    expect(VerifyVersionInfoA(vi, 3, both(3)) == 0 && last_error() == pehost::ERROR_OLD_WIN_VERSION, "but not 11.0, which fails with ERROR_OLD_WIN_VERSION");
    fill(10, 1, 0, 0, 0, 0, 0, 0);
    expect(VerifyVersionInfoA(vi, 3, both(3)) == 0, "and not 10.1: equal major, lower minor");
    fill(10, 0, 0, 0, 0, 0, 0, 0);
    expect(VerifyVersionInfoA(vi, 3, both(2)) == 0 && VerifyVersionInfoA(vi, 3, both(4)) == 0, "strictly greater and strictly less than itself fail");
    fill(9, 99, 0, 0, 0, 0, 0, 0);
    expect(VerifyVersionInfoA(vi, 3, both(2)) == 1, "9.99 is below 10.0 so strictly greater than it passes");
    fill(11, 0, 0, 0, 0, 0, 0, 0);
    expect(VerifyVersionInfoA(vi, 3, both(4)) == 1, "11.0 is above so strictly less than it passes");
    fill(10, 1, 0, 0, 0, 0, 0, 0);
    expect(VerifyVersionInfoA(vi, 3, both(4)) == 1 && VerifyVersionInfoA(vi, 3, both(5)) == 1, "10.1 is less than 10.0 with equal major, and less or equal");
    fill(10, 0, 0, 0, 0, 0, 0, 0);
    expect(VerifyVersionInfoA(vi, 3, both(5)) == 1 && VerifyVersionInfoA(vi, 2, VerSetConditionMask(0, 2, 1)) == 1, "less or equal and equal hold for the same version");
    fill(6, 0, 0, 0, 0, 0, 0, 0);
    expect(VerifyVersionInfoA(vi, 2, VerSetConditionMask(0, 2, 1)) == 0 && VerifyVersionInfoA(vi, 2, 0) == 0 && last_error() == pehost::ERROR_OLD_WIN_VERSION, "equality on the wrong version fails and a field without a condition fails");
    fill(0, 0, 19045, 2, 0, 0, 0, 1);
    expect(VerifyVersionInfoA(vi, 4, VerSetConditionMask(0, 4, 3)) == 1 && VerifyVersionInfoA(vi, 8, VerSetConditionMask(0, 8, 1)) == 1 && VerifyVersionInfoA(vi, 0x80, VerSetConditionMask(0, 0x80, 1)) == 1 && VerifyVersionInfoA(vi, 0x20, VerSetConditionMask(0, 0x20, 3)) == 1,
           "the build number, platform, product type and service pack of this system");
    fill(0, 0, 22000, 1, 1, 0, 1, 3);
    expect(VerifyVersionInfoA(vi, 4, VerSetConditionMask(0, 4, 3)) == 0 && VerifyVersionInfoA(vi, 8, VerSetConditionMask(0, 8, 1)) == 0 && VerifyVersionInfoA(vi, 0x80, VerSetConditionMask(0, 0x80, 1)) == 0 && VerifyVersionInfoA(vi, 0x20, VerSetConditionMask(0, 0x20, 3)) == 0 && VerifyVersionInfoA(vi, 0x40, 0) == 0,
           "a newer build, another platform, product type, service pack and a suite all fail");
    fill(0, 0, 0, 0, 0, 0, 0, 0);
    expect(VerifyVersionInfoA(vi, 0x40, 0) == 1, "a suite mask of zero is satisfied");
    set_last_error(0);
    *reinterpret_cast<u32*>(vi) = 100;
    expect(VerifyVersionInfoA(vi, 2, 0) == 0 && last_error() == pehost::ERROR_INVALID_PARAMETER && VerifyVersionInfoA(nullptr, 2, 0) == 0 && VerifyVersionInfoA(vi, 0, 0) == 0 && VerifyVersionInfoA(vi, 0x100, 0) == 0, "a short structure, null and bad type masks are invalid");
    *reinterpret_cast<u32*>(vi) = 148;
    expect(VerifyVersionInfoA(vi, 0x20, 0) == 0 && last_error() == pehost::ERROR_INVALID_PARAMETER && VerifyVersionInfoA(vi, 3, both(3)) == 1, "the extended fields need the extended structure");

    char fm[100];
    set_last_error(0);
    expect(FormatMessageA(0x1200, nullptr, 5, 0, fm, 100, nullptr) == 19 && same(fm, "Access is denied.\r\n") && FormatMessageA(0x1200, nullptr, 2, 0x409, fm, 100, nullptr) == detail::text_length(fm), "system messages are formatted into the buffer");
    char* allocated = nullptr;
    u32 alen = FormatMessageA(0x1300, nullptr, 50, 0, reinterpret_cast<char*>(&allocated), 0, nullptr);
    expect(allocated != nullptr && alen == detail::text_length(allocated) && contains(allocated, "not supported") && LocalFree(reinterpret_cast<u64>(allocated)) == 0, "the allocate flag returns a heap block that LocalFree releases");
    expect(FormatMessageA(0x1200, nullptr, 9999, 0, fm, 100, nullptr) == 0 && last_error() == pehost::ERROR_MR_MID_NOT_FOUND && FormatMessageA(0x1200, nullptr, 5, 0, fm, 10, nullptr) == 0 && last_error() == pehost::ERROR_INSUFFICIENT_BUFFER &&
               FormatMessageA(0x200, nullptr, 5, 0, fm, 100, nullptr) == 0 && last_error() == pehost::ERROR_INVALID_PARAMETER && FormatMessageA(0x400, nullptr, 5, 0, fm, 100, nullptr) == 0 && last_error() == pehost::ERROR_NOT_SUPPORTED,
           "an unknown message, a short buffer, a missing source and a string source are all reported");

    set_last_error(0);
    expect(CreateProcessA(nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, nullptr, nullptr, nullptr) == 0 && last_error() == pehost::ERROR_NOT_SUPPORTED && CreateNamedPipeA("x", 0, 0, 0, 0, 0, 0, nullptr) == INVALID_HANDLE && CreateIoCompletionPort(INVALID_HANDLE, 0, 0, 0) == 0 &&
               CreatePipe(nullptr, nullptr, nullptr, 0) == 0 && GetProcessId(1) == 0 && GetExitCodeProcess(1, nullptr) == 0 && ReleaseSemaphore(1, 1, nullptr) == 0 && OpenSemaphoreA(0, 0, "x") == 0,
           "the process, pipe and completion port stubs fail with ERROR_NOT_SUPPORTED");
    expect(WaitForSingleObject(1, 0) == 0xFFFFFFFFu && last_error() == pehost::ERROR_INVALID_HANDLE && GetDriveTypeW(wide("C:\\")) == 1, "waiting on an unknown handle fails and drives are of unknown type");

    namespace vc = pehost::vcrt;
    const char hay[] = "abcabc";
    expect(vc::memchr(hay, 'c', 6) == hay + 2 && vc::memchr(hay, 'z', 6) == nullptr && vc::memchr(hay, 'c', 2) == nullptr && vc::memchr(hay, 0, 7) == hay + 6 && vc::memchr(hay, 0x163, 6) == hay + 2, "memchr finds the first byte, honours the length and compares as a byte");
    const u16 wh[] = { 'a', 'b', 'a', 0 };
    expect(vc::wcschr(wh, 'a') == wh && vc::wcschr(wh, 0) == wh + 3 && vc::wcschr(wh, 'z') == nullptr && vc::wcsrchr(wh, 'a') == wh + 2 && vc::wcsrchr(wh, 0) == wh + 3 && vc::wcsrchr(wh, 'z') == nullptr, "wcschr and wcsrchr");
    expect(vc::wcsstr(wh, wide("ba")) == wh + 1 && vc::wcsstr(wh, wide("")) == wh && vc::wcsstr(wh, wide("abab")) == nullptr && vc::wcsstr(wh, wide("aba")) == wh && vc::wcsstr(wh, wide("bb")) == nullptr, "wcsstr");
    vc::ExceptionData src{ "boom", 1 };
    vc::ExceptionData dst{ reinterpret_cast<const char*>(1), 7 };
    vc::__std_exception_copy(&src, &dst);
    expect(dst.what != src.what && same(dst.what, "boom") && dst.free_it == 1, "copying an owned exception message duplicates it");
    vc::__std_exception_destroy(&dst);
    expect(dst.what == nullptr && dst.free_it == 0, "and destroying it releases it");
    src.free_it = 0;
    vc::__std_exception_copy(&src, &dst);
    expect(dst.what == src.what && dst.free_it == 0, "a borrowed message is shared");
    vc::__std_exception_destroy(&dst);
    expect(src.what != nullptr, "and destroying the copy leaves the original alone");
    src.what = nullptr;
    vc::__std_exception_copy(&src, &dst);
    expect(dst.what == nullptr, "an empty message copies as empty");
    FakeTypeInfo ta{ nullptr, nullptr, ".?AVfoo@@" }, tb{ nullptr, nullptr, ".?AVfoo@@" }, tc{ nullptr, nullptr, ".?AVbar@@" };
    expect(vc::__std_type_info_compare(&ta, &ta) == 0 && vc::__std_type_info_compare(&ta, &tb) == 0 && vc::__std_type_info_compare(&ta, &tc) > 0 && vc::__std_type_info_compare(&tc, &ta) < 0, "type information compares by decorated name");
    expect(vc::__std_type_info_hash(&ta) == vc::__std_type_info_hash(&tb) && vc::__std_type_info_hash(&ta) != vc::__std_type_info_hash(&tc), "and hashes by it");
    expect(vc::_get_purecall_handler() == nullptr && vc::_set_purecall_handler(reinterpret_cast<void*>(0x10)) == nullptr && vc::_set_purecall_handler(nullptr) == reinterpret_cast<void*>(0x10) && vc::_set_se_translator(reinterpret_cast<void*>(0x20)) == nullptr &&
               vc::_set_se_translator(nullptr) == reinterpret_cast<void*>(0x20) && vc::__uncaught_exception() == 0 && vc::__uncaught_exceptions() == 0 && vc::__processing_throw() != nullptr && vc::__current_exception() != vc::__current_exception_context() &&
               vc::__CxxFrameHandler3(nullptr, nullptr, nullptr, nullptr) == 1 && vc::__CxxFrameHandler4(nullptr, nullptr, nullptr, nullptr) == 1,
           "handler registration returns the previous handler and the exception state accessors exist");
    pehost::g_runtime = &g_child_runtime;
    ChildResult purecall = in_child([] { vc::_purecall(); });
    ChildResult overrun = in_child([] { vc::__report_gsfailure(0); });
    ChildResult thrown = in_child([] { vc::_CxxThrowException(nullptr, nullptr); });
    ChildResult terminated = in_child([] { vc::__std_terminate(); });
    pehost::g_runtime = &ext;
    expect(purecall.exited && purecall.code == 3 && contains(purecall.out, "pure virtual") && overrun.exited && static_cast<u32>(overrun.code) == 0xC0000409u && contains(overrun.out, "stack buffer overrun"), "a pure virtual call and a stack overrun end the process with their codes and a message");
    expect(thrown.exited && static_cast<u32>(thrown.code) == 0xE06D7363u && contains(thrown.out, "C++ exceptions are not supported") && terminated.exited && terminated.code == 3 && contains(terminated.out, "terminate it"), "a thrown C++ exception and std::terminate end the process and say why");

    u64 address = 0;
    const pehost::ImportTable* all_tables = pehost::all_imports();
    auto resolved = [&](const char* dll, const char* name) { return pehost::detail::resolve(const_cast<pehost::ImportTable*>(all_tables), dll, name, 0, &address); };
    expect(resolved("kernel32.dll", "FlsAlloc") && address == reinterpret_cast<u64>(&FlsAlloc) && resolved("api-ms-win-core-fibers-l1-1-1.dll", "FlsAlloc") && resolved("KERNELBASE.dll", "CreateFileW") && address == reinterpret_cast<u64>(&CreateFileW) &&
               resolved("kernel32.dll", "CreateProcessA") && address == reinterpret_cast<u64>(&CreateProcessA), "the new kernel32 functions and the failing stubs resolve by name and through api sets");
    expect(resolved("vcruntime140.dll", "memchr") && address == reinterpret_cast<u64>(&vc::memchr) && resolved("VCRUNTIME140_1.dll", "__CxxFrameHandler4") && resolved("vcruntime140.dll", "memcpy") && address == reinterpret_cast<u64>(&pehost::msvcrt::memcpy) &&
               !resolved("vcruntime140.dll", "FlsAlloc") && !resolved("vcruntime140.dll", "printf"), "vcruntime140 and its x64 companion resolve their own functions and the shared memory routines only");
    expect(pe::modules::provided("vcruntime140.dll") && same(pe::modules::canonical("VCRUNTIME140_1.dll"), "vcruntime140.dll") && same(pe::modules::canonical("vcruntime140.dll"), "vcruntime140.dll") && !pe::modules::provided("vcruntime140_1.dll"), "the module tables treat both vcruntime names as one provided module");
    u64 vcrt_module = GetModuleHandleA("vcruntime140.dll");
    expect(vcrt_module != 0 && vcrt_module != k32 && GetModuleHandleA("VCRUNTIME140_1.DLL") == vcrt_module && LoadLibraryA("vcruntime140.dll") == vcrt_module && GetProcAddress(vcrt_module, "memchr") == reinterpret_cast<void*>(&vc::memchr) && GetProcAddress(vcrt_module, "FlsAlloc") == nullptr,
           "vcruntime140 is a module with its own handle and function lookup");

    static u8 file[0x10000];
    usize file_size = 0;
    decode_b64(pe_sample_mingw_k32, file, &file_size);
    static pe::Image img;
    pe::parse(file, file_size, &img);
    u8* image_base = map_pages(img.size_of_image + 0x20000);
    image_base = reinterpret_cast<u8*>((reinterpret_cast<u64>(image_base) + 0xFFFF) & ~0xFFFFull);
    pe::map(img, file, file_size, image_base, img.size_of_image);
    pehost::Prepared prepared{};
    prepared.image = img;
    prepared.base = image_base;
    prepared.peb = &peb;
    pehost::attach(prepared, pehost::all_imports(), pehost::Config{ "C:\\dir\\prog.exe arg", "C:\\dir\\prog.exe" });
    pehost::ProcessParameters* pp = reinterpret_cast<pehost::ProcessParameters*>(peb.process_parameters);
    expect(pp != nullptr && pp->standard_input == HANDLE_STDIN && pp->standard_output == HANDLE_STDOUT && pp->standard_error == HANDLE_STDERR && pp->length == sizeof(*pp), "the PEB carries process parameters with the standard handles");
    expect(pp && pp->command_line.length == 19 * 2 && wide_is(reinterpret_cast<const u16*>(pp->command_line.buffer), "C:\\dir\\prog.exe arg") && pp->image_path_name.length == 15 * 2 && wide_is(reinterpret_cast<const u16*>(pp->image_path_name.buffer), "C:\\dir\\prog.exe") &&
               pp->current_directory.length == 2 && wide_is(reinterpret_cast<const u16*>(pp->current_directory.buffer), "\\") && pp->environment != 0 && pp->dll_path.length == 0,
           "the command line, image path, current directory, DLL path and environment are filled in");
    pehost::g_runtime = &g_recording;
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
    extended_tests();
    module_tests();
    printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
