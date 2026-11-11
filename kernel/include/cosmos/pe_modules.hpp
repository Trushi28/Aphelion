#pragma once
#include <cosmos/types.hpp>

namespace pe::modules {

inline char fold(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

inline bool starts(const char* s, const char* prefix) {
    while (*prefix) {
        if (fold(*s) != *prefix) return false;
        ++s;
        ++prefix;
    }
    return true;
}

inline bool ends(const char* s, const char* suffix) {
    u64 n = 0, k = 0;
    while (s[n]) ++n;
    while (suffix[k]) ++k;
    if (n < k) return false;
    for (u64 i = 0; i < k; ++i) {
        if (fold(s[n - k + i]) != suffix[i]) return false;
    }
    return true;
}

inline bool is(const char* s, const char* whole) {
    while (*whole) {
        if (fold(*s) != *whole) return false;
        ++s;
        ++whole;
    }
    return *s == 0;
}

inline bool provided(const char* home) { return is(home, "kernel32.dll") || is(home, "msvcrt.dll") || is(home, "ucrtbase.dll") || is(home, "vcruntime140.dll"); }

inline const char* canonical(const char* dll) {
    if (is(dll, "kernel32.dll") || is(dll, "kernelbase.dll")) return "kernel32.dll";
    if (is(dll, "msvcrt.dll")) return "msvcrt.dll";
    if (is(dll, "ucrtbase.dll")) return "ucrtbase.dll";
    if (is(dll, "vcruntime140.dll") || is(dll, "vcruntime140_1.dll")) return "vcruntime140.dll";
    if (ends(dll, ".dll")) {
        if (starts(dll, "api-ms-win-crt-")) return "ucrtbase.dll";
        if (starts(dll, "api-ms-win-") || starts(dll, "ext-ms-win-")) return "kernel32.dll";
    }
    return dll;
}

}
