#pragma once
#include <cosmos/types.hpp>

namespace corona::font {

constexpr u32 FACE_COUNT = 3;
constexpr u32 NO_GLYPH = 0xFFFFFFFFu;
constexpr u32 FALLBACK = 0xFFFD;

struct Face {
    u16 cell_w;
    u16 cell_h;
    u16 baseline;
    u32 count;
    const u32* codepoints;
    const u32* regular;
    const u32* bold;
    const u8* bits;
};

bool load(const u8* blob, usize length);
u32 face_count();
const Face& face(u32 index);
u32 pick(u64 fb_width, u64 fb_height);

i32 find(const Face& f, u32 cp);
const u8* glyph(const Face& f, u32 cp, bool bold, bool* is_fallback);
bool has(const Face& f, u32 cp);

constexpr u32 glyph_bytes(const Face& f) { return static_cast<u32>(f.cell_w) * f.cell_h / 2; }

ALWAYS_INLINE u8 coverage(const u8* g, u32 index) {
    u8 b = g[index >> 1];
    return (index & 1) ? static_cast<u8>(b >> 4) : static_cast<u8>(b & 0x0F);
}

}
