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

}
