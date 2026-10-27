#pragma once
#include <cosmos/types.hpp>
#include <cosmos/stellar.hpp>

namespace boottest {

constexpr u64 TEST_TAIL_SECTORS = 64;

inline u32 itoa_dec(u64 v, char* out) {
    char tmp[20];
    u32 len = 0;
    if (v == 0) tmp[len++] = '0';
    while (v) { tmp[len++] = static_cast<char>('0' + (v % 10)); v /= 10; }
    for (u32 i = 0; i < len; ++i) out[i] = tmp[len - 1 - i];
    return len;
}

inline void count_entry(const char*, u64, u32, void* ctx) {
    ++(*static_cast<u64*>(ctx));
}

inline u64 cstr_len(const char* s) {
    u64 n = 0;
    while (s[n]) ++n;
    return n;
}

inline bool file_equals(u64 star, const char* expect, u64 snap = stellar::LIVE) {
    u8 tmp[256];
    u64 want = cstr_len(expect);
    if (star == stellar::INVALID_STAR || want >= sizeof(tmp)) return false;
    u64 got = stellar::read_file(star, tmp, sizeof(tmp), snap);
    if (got != want) return false;
    for (u64 i = 0; i < got; ++i)
        if (tmp[i] != static_cast<u8>(expect[i])) return false;
    return true;
}

void run_fs_selftests(bool fresh_fs);
void arm_fs_stress(u64 dir, u64 alt);
void spawn_test_tasks();
void spawn_mm_stress();

}
