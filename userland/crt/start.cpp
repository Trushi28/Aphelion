#include <aphelion.hpp>

asm(R"(
.section .text.start,"ax",@progbits
.globl _start
.type _start,@function
_start:
    xorl %ebp, %ebp
    andq $-16, %rsp
    call aphelion_main
    movq %rax, %rdi
    xorl %eax, %eax
    syscall
    ud2
.size _start, .-_start
)");

extern "C" void* memset(void* dst, int value, u64 n) {
    u8* p = static_cast<u8*>(dst);
    for (u64 i = 0; i < n; ++i) p[i] = static_cast<u8>(value);
    return dst;
}

extern "C" void* memcpy(void* dst, const void* src, u64 n) {
    u8* d = static_cast<u8*>(dst);
    const u8* s = static_cast<const u8*>(src);
    for (u64 i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}

extern "C" void* memmove(void* dst, const void* src, u64 n) {
    u8* d = static_cast<u8*>(dst);
    const u8* s = static_cast<const u8*>(src);
    if (d < s) {
        for (u64 i = 0; i < n; ++i) d[i] = s[i];
    } else {
        for (u64 i = n; i > 0; --i) d[i - 1] = s[i - 1];
    }
    return dst;
}

extern "C" int memcmp(const void* a, const void* b, u64 n) {
    const u8* x = static_cast<const u8*>(a);
    const u8* y = static_cast<const u8*>(b);
    for (u64 i = 0; i < n; ++i) {
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    }
    return 0;
}
