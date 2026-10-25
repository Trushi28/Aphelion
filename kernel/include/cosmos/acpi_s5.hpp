#pragma once
#include <cosmos/types.hpp>

namespace acpi {

inline bool s5_element(const u8* aml, u32 len, u32& p, u8& out) {
    if (p >= len) return false;
    u32 value;
    switch (aml[p]) {
        case 0x00: value = 0; p += 1; break;
        case 0x01: value = 1; p += 1; break;
        case 0x0A:
            if (p + 1 >= len) return false;
            value = aml[p + 1];
            p += 2;
            break;
        case 0x0B:
            if (p + 2 >= len) return false;
            value = static_cast<u32>(aml[p + 1]) | (static_cast<u32>(aml[p + 2]) << 8);
            p += 3;
            break;
        default: return false;
    }
    if (value > 7) return false;
    out = static_cast<u8>(value);
    return true;
}

inline bool find_s5(const u8* aml, u32 len, u8* typ_a, u8* typ_b) {
    if (len < 8) return false;
    for (u32 i = 1; i + 5 < len; ++i) {
        if (aml[i] != '_' || aml[i + 1] != 'S' || aml[i + 2] != '5' || aml[i + 3] != '_') continue;
        bool named = aml[i - 1] == 0x08 || (i >= 2 && aml[i - 1] == '\\' && aml[i - 2] == 0x08);
        if (!named) continue;
        u32 p = i + 4;
        if (aml[p] != 0x12) continue;
        ++p;
        if (p >= len) continue;
        p += 1u + (aml[p] >> 6);
        if (p >= len || aml[p] < 2) continue;
        ++p;
        u8 a, b;
        if (!s5_element(aml, len, p, a) || !s5_element(aml, len, p, b)) continue;
        *typ_a = a;
        *typ_b = b;
        return true;
    }
    return false;
}

}
