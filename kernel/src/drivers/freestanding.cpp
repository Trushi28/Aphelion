#include <cosmos/types.hpp>

extern "C" void* memset(void* dst, int v, usize n) {
    void* d = dst;
    u8 byte = static_cast<u8>(v);
    u64 pattern = static_cast<u64>(byte) * 0x0101010101010101ull;
    u64 quads = n >> 3;
    u64 rest = n & 7;
    asm volatile("rep stosq" : "+D"(d), "+c"(quads) : "a"(pattern) : "memory");
    asm volatile("rep stosb" : "+D"(d), "+c"(rest) : "a"(byte) : "memory");
    return dst;
}

extern "C" void* memcpy(void* dst, const void* src, usize n) {
    void* d = dst;
    const void* s = src;
    u64 quads = n >> 3;
    u64 rest = n & 7;
    asm volatile("rep movsq" : "+D"(d), "+S"(s), "+c"(quads) :: "memory");
    asm volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(rest) :: "memory");
    return dst;
}

extern "C" void* memmove(void* dst, const void* src, usize n) {
    auto* d = static_cast<u8*>(dst);
    auto* s = static_cast<const u8*>(src);
    if (d < s) {
        void* dd = d;
        const void* ss = s;
        u64 quads = n >> 3;
        u64 rest = n & 7;
        asm volatile("rep movsq" : "+D"(dd), "+S"(ss), "+c"(quads) :: "memory");
        asm volatile("rep movsb" : "+D"(dd), "+S"(ss), "+c"(rest) :: "memory");
    } else {
        for (usize i = n; i-- > 0;) d[i] = s[i];
    }
    return dst;
}

extern "C" int memcmp(const void* a, const void* b, usize n) {
    auto* pa = static_cast<const u8*>(a);
    auto* pb = static_cast<const u8*>(b);
    for (usize i = 0; i < n; ++i)
        if (pa[i] != pb[i]) return pa[i] - pb[i];
    return 0;
}

extern "C" usize strlen(const char* s) {
    usize n = 0;
    while (s[n]) ++n;
    return n;
}

extern "C" void __stack_chk_fail(void) { for (;;) asm volatile("cli; hlt"); }
extern "C" unsigned long __stack_chk_guard;
unsigned long __stack_chk_guard = 0xdeadbeefcafebabe;
