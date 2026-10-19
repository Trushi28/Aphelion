#include <cosmos/corona_font.hpp>

namespace corona::font {

namespace {

constexpr u32 MAGIC = 0x414E5243;
constexpr u32 VERSION = 1;
constexpr u32 DESCRIPTOR_WORDS = 8;

Face g_faces[FACE_COUNT];
u32 g_count = 0;

u32 word(const u8* p) {
    u32 v;
    memcpy(&v, p, 4);
    return v;
}

}

bool load(const u8* blob, usize length) {
    g_count = 0;
    if (!blob || length < 12) return false;
    if (word(blob) != MAGIC || word(blob + 4) != VERSION) return false;
    u32 faces = word(blob + 8);
    if (faces == 0 || faces > FACE_COUNT) return false;
    if (length < 12 + static_cast<usize>(faces) * DESCRIPTOR_WORDS * 4) return false;

    for (u32 i = 0; i < faces; ++i) {
        const u8* d = blob + 12 + i * DESCRIPTOR_WORDS * 4;
        u32 w = word(d), h = word(d + 4), base = word(d + 8), n = word(d + 12);
        u32 off_cps = word(d + 16), off_reg = word(d + 20), off_bold = word(d + 24), off_bits = word(d + 28);
        if (w == 0 || h == 0 || (w * h) % 2 != 0 || base >= h || n == 0) return false;
        usize table = static_cast<usize>(n) * 4;
        if (off_cps % 4 || off_reg % 4 || off_bold % 4) return false;
        if (off_cps + table > length || off_reg + table > length || off_bold + table > length || off_bits > length) return false;
        Face& f = g_faces[i];
        f.cell_w = static_cast<u16>(w);
        f.cell_h = static_cast<u16>(h);
        f.baseline = static_cast<u16>(base);
        f.count = n;
        f.codepoints = reinterpret_cast<const u32*>(blob + off_cps);
        f.regular = reinterpret_cast<const u32*>(blob + off_reg);
        f.bold = reinterpret_cast<const u32*>(blob + off_bold);
        f.bits = blob + off_bits;
        usize glyph = static_cast<usize>(w) * h / 2;
        for (u32 k = 0; k < n; ++k) {
            if (k && f.codepoints[k] <= f.codepoints[k - 1]) return false;
            if (f.regular[k] == NO_GLYPH || static_cast<usize>(f.regular[k]) + glyph > length - off_bits) return false;
            if (f.bold[k] != NO_GLYPH && static_cast<usize>(f.bold[k]) + glyph > length - off_bits) return false;
        }
        if (find(f, FALLBACK) < 0) return false;
    }
    g_count = faces;
    return true;
}

u32 face_count() { return g_count; }

const Face& face(u32 index) { return g_faces[index < g_count ? index : 0]; }

u32 pick(u64 fb_width, u64 fb_height) {
    if (g_count == 0) return 0;
    u32 want = 0;
    if (fb_width >= 2300 && fb_height >= 1300) want = 2;
    else if (fb_width >= 1500 && fb_height >= 900) want = 1;
    return want < g_count ? want : g_count - 1;
}

i32 find(const Face& f, u32 cp) {
    i32 lo = 0, hi = static_cast<i32>(f.count) - 1;
    while (lo <= hi) {
        i32 mid = lo + (hi - lo) / 2;
        u32 v = f.codepoints[mid];
        if (v == cp) return mid;
        if (v < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

bool has(const Face& f, u32 cp) { return find(f, cp) >= 0; }

const u8* glyph(const Face& f, u32 cp, bool bold, bool* is_fallback) {
    i32 i = find(f, cp);
    bool fb = false;
    if (i < 0) {
        i = find(f, FALLBACK);
        fb = true;
    }
    if (is_fallback) *is_fallback = fb;
    u32 off = f.regular[i];
    if (bold && !fb && f.bold[i] != NO_GLYPH) off = f.bold[i];
    return f.bits + off;
}

}
