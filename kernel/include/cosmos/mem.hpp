#pragma once

namespace mem {

constexpr void copy(void* dst, const void* src, decltype(sizeof(0)) n) { __builtin_memcpy(dst, src, n); }
constexpr void move(void* dst, const void* src, decltype(sizeof(0)) n) { __builtin_memmove(dst, src, n); }
constexpr void fill(void* dst, unsigned char value, decltype(sizeof(0)) n) { __builtin_memset(dst, value, n); }
constexpr void zero(void* dst, decltype(sizeof(0)) n) { __builtin_memset(dst, 0, n); }
constexpr int compare(const void* a, const void* b, decltype(sizeof(0)) n) { return __builtin_memcmp(a, b, n); }
constexpr bool equal(const void* a, const void* b, decltype(sizeof(0)) n) { return n == 0 || __builtin_memcmp(a, b, n) == 0; }

}

namespace cstr {

constexpr decltype(sizeof(0)) length(const char* s) { return __builtin_strlen(s); }
constexpr int compare(const char* a, const char* b) { return __builtin_strcmp(a, b); }
constexpr bool equal(const char* a, const char* b) { return __builtin_strcmp(a, b) == 0; }
constexpr char* copy(char* dst, const char* src) { return __builtin_strcpy(dst, src); }
constexpr const char* find(const char* hay, const char* needle) { return __builtin_strstr(hay, needle); }

}
