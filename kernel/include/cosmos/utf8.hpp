#pragma once
#include <cosmos/types.hpp>

namespace utf8 {

constexpr u32 REPLACEMENT = 0xFFFD;
constexpr u32 MAX_CODEPOINT = 0x10FFFF;

constexpr bool valid_scalar(u32 cp) {
    return cp <= MAX_CODEPOINT && !(cp >= 0xD800 && cp <= 0xDFFF);
}

constexpr u32 encode(u32 cp, char out[4]) {
    if (!valid_scalar(cp)) cp = REPLACEMENT;
    if (cp < 0x80) {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
}

class Decoder {
public:
    u32 feed(u8 b, u32 out[2]) {
        if (need_ == 0) return start(b, out);
        if ((b & 0xC0) != 0x80) {
            u32 n = 0;
            out[n++] = REPLACEMENT;
            reset();
            n += start(b, out + n);
            return n;
        }
        acc_ = (acc_ << 6) | (b & 0x3F);
        if (--need_ > 0) return 0;
        u32 cp = acc_;
        u32 min = min_;
        reset();
        if (cp < min || !valid_scalar(cp)) {
            out[0] = REPLACEMENT;
            return 1;
        }
        out[0] = cp;
        return 1;
    }

    u32 finish(u32 out[1]) {
        if (need_ == 0) return 0;
        reset();
        out[0] = REPLACEMENT;
        return 1;
    }

    bool pending() const { return need_ != 0; }

private:
    void reset() { need_ = 0; acc_ = 0; min_ = 0; }

    u32 start(u8 b, u32 out[2]) {
        if (b < 0x80) { out[0] = b; return 1; }
        if (b >= 0xC2 && b <= 0xDF) { need_ = 1; acc_ = b & 0x1F; min_ = 0x80; return 0; }
        if (b >= 0xE0 && b <= 0xEF) { need_ = 2; acc_ = b & 0x0F; min_ = 0x800; return 0; }
        if (b >= 0xF0 && b <= 0xF4) { need_ = 3; acc_ = b & 0x07; min_ = 0x10000; return 0; }
        out[0] = REPLACEMENT;
        return 1;
    }

    u32 need_ = 0;
    u32 acc_ = 0;
    u32 min_ = 0;
};

struct Range { u32 lo; u32 hi; };

constexpr Range ZERO_WIDTH[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF}, {0x05C1, 0x05C2},
    {0x05C4, 0x05C5}, {0x05C7, 0x05C7}, {0x0610, 0x061A}, {0x064B, 0x065F}, {0x0670, 0x0670},
    {0x06D6, 0x06DC}, {0x06DF, 0x06E4}, {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x1AB0, 0x1AFF},
    {0x1DC0, 0x1DFF}, {0x200B, 0x200F}, {0x2028, 0x202E}, {0x2060, 0x2064}, {0x20D0, 0x20FF},
    {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF}, {0xE0100, 0xE01EF},
};

constexpr Range WIDE[] = {
    {0x1100, 0x115F}, {0x2E80, 0x303E}, {0x3041, 0x33FF}, {0x3400, 0x4DBF}, {0x4E00, 0xA4CF},
    {0xA960, 0xA97F}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF}, {0xFE30, 0xFE6F}, {0xFF00, 0xFF60},
    {0xFFE0, 0xFFE6}, {0x1F300, 0x1F64F}, {0x1F680, 0x1F6FF}, {0x1F900, 0x1F9FF}, {0x20000, 0x3FFFD},
};

template <usize N>
constexpr bool in_ranges(const Range (&r)[N], u32 cp) {
    for (usize i = 0; i < N; ++i)
        if (cp >= r[i].lo && cp <= r[i].hi) return true;
    return false;
}

constexpr u32 width(u32 cp) {
    if (cp == 0) return 0;
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;
    if (cp < 0x300) return 1;
    if (in_ranges(ZERO_WIDTH, cp)) return 0;
    if (in_ranges(WIDE, cp)) return 2;
    return 1;
}

inline u32 cells(const char* s, u32 n) {
    Decoder d;
    u32 out[2];
    u32 count = 0;
    for (u32 i = 0; i < n; ++i) {
        u32 k = d.feed(static_cast<u8>(s[i]), out);
        for (u32 j = 0; j < k; ++j) count += width(out[j]) ? 1 : 0;
    }
    u32 tail[1];
    count += d.finish(tail);
    return count;
}

}
