#pragma once
#include <cosmos/types.hpp>

namespace entrycheck {

constexpr u32 FENCE_REACH = 8;

constexpr bool is_swapgs(const u8* p) { return p[0] == 0x0F && p[1] == 0x01 && p[2] == 0xF8; }
constexpr bool is_lfence(const u8* p) { return p[0] == 0x0F && p[1] == 0xAE && p[2] == 0xE8; }

enum class Verdict : u8 { Fenced, NoSwapgs, Unfenced };

constexpr Verdict first_swapgs(const u8* code, u32 window) {
    if (window < 3) return Verdict::NoSwapgs;
    for (u32 i = 0; i + 3 <= window; ++i) {
        if (!is_swapgs(code + i)) continue;
        u32 end = i + 3 + FENCE_REACH;
        if (end + 3 > window) end = window - 3;
        for (u32 j = i + 3; j <= end; ++j) {
            if (is_lfence(code + j)) return Verdict::Fenced;
        }
        return Verdict::Unfenced;
    }
    return Verdict::NoSwapgs;
}

}
