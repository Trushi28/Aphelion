#pragma once
#include <cosmos/types.hpp>

namespace pehost::codec {

constexpr i64 INVALID = -1;
constexpr i64 TOO_SMALL = -2;

namespace detail {

struct Decoder {
    const u8* s;
    u64 n;
    u64 at = 0;
    bool bad = false;

    bool done() const { return at >= n; }

    u32 next() {
        u8 lead = s[at++];
        if (lead < 0x80) return lead;
        u32 need;
        u32 code;
        u8 lo = 0x80, hi = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            need = 1;
            code = lead & 0x1F;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            need = 2;
            code = lead & 0x0F;
            if (lead == 0xE0) lo = 0xA0;
            if (lead == 0xED) hi = 0x9F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            need = 3;
            code = lead & 0x07;
            if (lead == 0xF0) lo = 0x90;
            if (lead == 0xF4) hi = 0x8F;
        } else {
            bad = true;
            return 0xFFFD;
        }
        while (need) {
            if (at >= n || s[at] < lo || s[at] > hi) {
                bad = true;
                return 0xFFFD;
            }
            code = (code << 6) | (s[at++] & 0x3F);
            lo = 0x80;
            hi = 0xBF;
            --need;
        }
        return code;
    }
};

}

inline i64 utf8_to_utf16(const u8* s, u64 n, u16* out, u64 cap, bool strict) {
    detail::Decoder d{ s, n };
    u64 w = 0;
    while (!d.done()) {
        d.bad = false;
        u32 c = d.next();
        if (d.bad && strict) return INVALID;
        u32 units = c >= 0x10000 ? 2 : 1;
        if (out) {
            if (w + units > cap) return TOO_SMALL;
            if (units == 2) {
                c -= 0x10000;
                out[w] = static_cast<u16>(0xD800 + (c >> 10));
                out[w + 1] = static_cast<u16>(0xDC00 + (c & 0x3FF));
            } else {
                out[w] = static_cast<u16>(c);
            }
        }
        w += units;
    }
    return static_cast<i64>(w);
}

inline i64 utf16_to_utf8(const u16* s, u64 n, u8* out, u64 cap, bool strict) {
    u64 w = 0;
    u64 i = 0;
    while (i < n) {
        u32 c = s[i++];
        if (c >= 0xD800 && c <= 0xDBFF && i < n && s[i] >= 0xDC00 && s[i] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i] - 0xDC00);
            ++i;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            if (strict) return INVALID;
            c = 0xFFFD;
        }
        u32 bytes = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (out) {
            if (w + bytes > cap) return TOO_SMALL;
            if (bytes == 1) {
                out[w] = static_cast<u8>(c);
            } else if (bytes == 2) {
                out[w] = static_cast<u8>(0xC0 | (c >> 6));
                out[w + 1] = static_cast<u8>(0x80 | (c & 0x3F));
            } else if (bytes == 3) {
                out[w] = static_cast<u8>(0xE0 | (c >> 12));
                out[w + 1] = static_cast<u8>(0x80 | ((c >> 6) & 0x3F));
                out[w + 2] = static_cast<u8>(0x80 | (c & 0x3F));
            } else {
                out[w] = static_cast<u8>(0xF0 | (c >> 18));
                out[w + 1] = static_cast<u8>(0x80 | ((c >> 12) & 0x3F));
                out[w + 2] = static_cast<u8>(0x80 | ((c >> 6) & 0x3F));
                out[w + 3] = static_cast<u8>(0x80 | (c & 0x3F));
            }
        }
        w += bytes;
    }
    return static_cast<i64>(w);
}

}
