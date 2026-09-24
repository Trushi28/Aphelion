#pragma once
#include <cosmos/types.hpp>

namespace idt {

struct PACKED Frame {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vector;
    u64 error_code;
    u64 rip, cs, rflags, rsp, ss;
};

using Handler = void (*)(Frame*);

void init();
void set_handler(u8 vector, Handler h);

constexpr u8 VEC_APIC_TIMER = 0x20;
constexpr u8 VEC_VIRTIO_BLK = 0x22;
constexpr u8 VEC_SPURIOUS   = 0xFF;

}
