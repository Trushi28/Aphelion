#include <cosmos/types.hpp>

extern "C" void* memset(void* dst, int v, usize n) {
    auto* d = static_cast<u8*>(dst);
    for (usize i = 0; i < n; ++i) d[i] = static_cast<u8>(v);
    return dst;
}

extern "C" void* memcpy(void* dst, const void* src, usize n) {
    auto* d = static_cast<u8*>(dst);
    auto* s = static_cast<const u8*>(src);
    for (usize i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}

extern "C" void* memmove(void* dst, const void* src, usize n) {
    auto* d = static_cast<u8*>(dst);
    auto* s = static_cast<const u8*>(src);
    if (d < s) { for (usize i = 0; i < n; ++i) d[i] = s[i]; }
    else       { for (usize i = n; i-- > 0;)   d[i] = s[i]; }
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
