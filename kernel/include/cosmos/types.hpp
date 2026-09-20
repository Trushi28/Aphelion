#pragma once

using u8  = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long long;
using i8  = signed char;
using i16 = signed short;
using i32 = signed int;
using i64 = signed long long;
using usize = u64;
using uptr = u64;

static_assert(sizeof(u64) == 8, "u64 must be 8 bytes");
static_assert(sizeof(void*) == 8, "kernel must be built 64-bit");

#define ALWAYS_INLINE inline __attribute__((always_inline))
#define PACKED __attribute__((packed))
#define NORETURN __attribute__((noreturn))
#define UNUSED __attribute__((unused))

inline void* operator new(__SIZE_TYPE__, void* p) noexcept { return p; }
inline void* operator new[](__SIZE_TYPE__, void* p) noexcept { return p; }

extern "C" void* memset(void* dst, int v, usize n);
extern "C" void* memcpy(void* dst, const void* src, usize n);
extern "C" void* memmove(void* dst, const void* src, usize n);
extern "C" int   memcmp(const void* a, const void* b, usize n);
extern "C" usize strlen(const char* s);
