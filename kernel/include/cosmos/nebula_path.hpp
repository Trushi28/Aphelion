#pragma once
#include <cosmos/types.hpp>

namespace nebula {
namespace path {

constexpr u32 MAX_COMPONENTS = 32;
constexpr u32 NAME_MAX = 51;
constexpr u32 REALM_MAX = 16;
constexpr u32 TEXT_MAX = 512;

enum class Error : u8 { Ok, Empty, BadRealm, BadName, NameTooLong, TooDeep, TooLong };

struct Path {
    char realm[REALM_MAX + 1];
    u32 count;
    char comps[MAX_COMPONENTS][NAME_MAX + 1];
};

inline bool realm_name_ok(const char* s, u32 n) {
    if (n == 0 || n > REALM_MAX) return false;
    if (s[0] < 'a' || s[0] > 'z') return false;
    for (u32 i = 0; i < n; ++i) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
    }
    return true;
}

inline bool forbidden_scalar(u32 cp) {
    if (cp < 0x20 || cp == 0x7F) return true;
    if (cp >= 0x80 && cp <= 0x9F) return true;
    if (cp == 0x2028 || cp == 0x2029) return true;
    if (cp >= 0x202A && cp <= 0x202E) return true;
    if (cp >= 0x2066 && cp <= 0x2069) return true;
    if (cp == 0xFEFF || cp == 0xFFFE || cp == 0xFFFF) return true;
    return false;
}

inline bool well_formed(const char* s, u32 n, bool* clean) {
    *clean = true;
    u32 i = 0;
    while (i < n) {
        u8 b = static_cast<u8>(s[i]);
        u32 need = 0, cp = 0, min = 0;
        if (b < 0x80) { cp = b; }
        else if (b >= 0xC2 && b <= 0xDF) { need = 1; cp = b & 0x1F; min = 0x80; }
        else if (b >= 0xE0 && b <= 0xEF) { need = 2; cp = b & 0x0F; min = 0x800; }
        else if (b >= 0xF0 && b <= 0xF4) { need = 3; cp = b & 0x07; min = 0x10000; }
        else return false;
        for (u32 k = 1; k <= need; ++k) {
            if (i + k >= n) return false;
            u8 c = static_cast<u8>(s[i + k]);
            if ((c & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (c & 0x3F);
        }
        if (need && (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) return false;
        if (forbidden_scalar(cp)) *clean = false;
        i += need + 1;
    }
    return true;
}

inline bool name_ok(const char* s, u32 n) {
    if (n == 0 || n > NAME_MAX) return false;
    bool clean = true;
    if (!well_formed(s, n, &clean) || !clean) return false;
    for (u32 i = 0; i < n; ++i) if (s[i] == '/' || s[i] == 0) return false;
    return true;
}

inline void clear(Path& p) {
    p.realm[0] = 0;
    p.count = 0;
}

inline u32 text_length(const char* s) {
    u32 n = 0;
    while (s[n]) ++n;
    return n;
}

inline void copy_text(char* dst, const char* src, u32 n) {
    for (u32 i = 0; i < n; ++i) dst[i] = src[i];
    dst[n] = 0;
}

inline bool equal(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

inline Error resolve(const Path& here, const char* text, Path& out) {
    if (!text || !*text) return Error::Empty;
    u32 len = text_length(text);
    if (len >= TEXT_MAX) return Error::TooLong;

    u32 start = 0;
    u32 colon = len;
    for (u32 i = 0; i < len && text[i] != '/'; ++i)
        if (text[i] == ':') { colon = i; break; }

    out = here;
    if (colon < len) {
        if (!realm_name_ok(text, colon)) return Error::BadRealm;
        copy_text(out.realm, text, colon);
        out.count = 0;
        start = colon + 1;
    } else if (text[0] == '/') {
        out.count = 0;
    }
    if (!out.realm[0]) return Error::BadRealm;

    u32 i = start;
    while (i < len) {
        while (i < len && text[i] == '/') ++i;
        if (i >= len) break;
        u32 j = i;
        while (j < len && text[j] != '/') ++j;
        u32 n = j - i;
        if (n == 1 && text[i] == '.') { i = j; continue; }
        if (n == 2 && text[i] == '.' && text[i + 1] == '.') {
            if (out.count > 0) --out.count;
            i = j;
            continue;
        }
        if (n > NAME_MAX) return Error::NameTooLong;
        if (!name_ok(text + i, n)) return Error::BadName;
        if (out.count >= MAX_COMPONENTS) return Error::TooDeep;
        copy_text(out.comps[out.count++], text + i, n);
        i = j;
    }
    return Error::Ok;
}

inline u32 to_text(const Path& p, char* out, u32 cap) {
    u32 n = 0;
    auto put = [&](char c) { if (n + 1 < cap) out[n] = c; ++n; };
    for (u32 i = 0; p.realm[i]; ++i) put(p.realm[i]);
    put(':');
    put('/');
    for (u32 c = 0; c < p.count; ++c) {
        if (c) put('/');
        for (u32 i = 0; p.comps[c][i]; ++i) put(p.comps[c][i]);
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
    return n;
}

inline bool join(Path& p, const char* name) {
    u32 n = text_length(name);
    if (!name_ok(name, n) || p.count >= MAX_COMPONENTS) return false;
    copy_text(p.comps[p.count++], name, n);
    return true;
}

}
}
