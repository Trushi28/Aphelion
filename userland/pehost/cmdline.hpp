#pragma once
#include <cosmos/types.hpp>

namespace pehost::cmdline {

inline bool blank(char c) { return c == ' ' || c == '\t'; }

inline bool parse(const char* line, char* storage, u32 capacity, char** argv, u32 max_args, u32* count) {
    u32 used = 0;
    u32 n = 0;
    *count = 0;
    const char* p = line;
    auto put = [&](char c) {
        if (used >= capacity) return false;
        storage[used++] = c;
        return true;
    };
    auto begin = [&]() {
        if (n + 1 >= max_args) return false;
        argv[n++] = storage + used;
        return true;
    };
    auto finish = [&]() { return put(0); };

    while (blank(*p)) ++p;
    if (*p == 0) {
        argv[0] = nullptr;
        return true;
    }
    if (!begin()) return false;
    if (*p == '"') {
        ++p;
        while (*p && *p != '"') {
            if (!put(*p++)) return false;
        }
        if (*p == '"') ++p;
    } else {
        while (*p && !blank(*p)) {
            if (!put(*p++)) return false;
        }
    }
    if (!finish()) return false;

    for (;;) {
        while (blank(*p)) ++p;
        if (*p == 0) break;
        if (!begin()) return false;
        bool quoted = false;
        while (*p) {
            u32 slashes = 0;
            while (*p == '\\') {
                ++slashes;
                ++p;
            }
            if (*p == '"') {
                for (u32 i = 0; i < slashes / 2; ++i) {
                    if (!put('\\')) return false;
                }
                if (slashes % 2 == 1) {
                    if (!put('"')) return false;
                    ++p;
                } else if (quoted && p[1] == '"') {
                    if (!put('"')) return false;
                    p += 2;
                } else {
                    quoted = !quoted;
                    ++p;
                }
                continue;
            }
            for (u32 i = 0; i < slashes; ++i) {
                if (!put('\\')) return false;
            }
            if (*p == 0 || (!quoted && blank(*p))) break;
            if (!put(*p++)) return false;
        }
        if (!finish()) return false;
    }
    argv[n] = nullptr;
    *count = n;
    return true;
}

inline bool needs_quotes(const char* a) {
    if (*a == 0) return true;
    for (; *a; ++a) {
        if (blank(*a) || *a == '"') return true;
    }
    return false;
}

inline bool join(const char* const* argv, u32 argc, char* out, u32 capacity) {
    u32 used = 0;
    auto put = [&](char c) {
        if (used + 1 >= capacity) return false;
        out[used++] = c;
        return true;
    };
    for (u32 i = 0; i < argc; ++i) {
        if (i > 0 && !put(' ')) return false;
        const char* a = argv[i];
        if (!needs_quotes(a)) {
            for (; *a; ++a) {
                if (!put(*a)) return false;
            }
            continue;
        }
        if (!put('"')) return false;
        if (i == 0) {
            for (; *a; ++a) {
                if (!put(*a)) return false;
            }
        } else {
            u32 slashes = 0;
            for (; *a; ++a) {
                if (*a == '\\') {
                    ++slashes;
                    if (!put('\\')) return false;
                    continue;
                }
                if (*a == '"') {
                    for (u32 k = 0; k < slashes; ++k) {
                        if (!put('\\')) return false;
                    }
                    if (!put('\\') || !put('"')) return false;
                } else if (!put(*a)) {
                    return false;
                }
                slashes = 0;
            }
            for (u32 k = 0; k < slashes; ++k) {
                if (!put('\\')) return false;
            }
        }
        if (!put('"')) return false;
    }
    out[used] = 0;
    return true;
}

}
