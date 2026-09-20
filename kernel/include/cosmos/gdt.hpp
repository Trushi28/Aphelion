#pragma once
#include <cosmos/types.hpp>

namespace gdt {

struct PACKED Entry {
    u16 limit_low;
    u16 base_low;
    u8  base_mid;
    u8  access;
    u8  granularity;
    u8  base_high;
};

struct PACKED TssEntry {
    u16 length;
    u16 base_low;
    u8  base_mid;
    u8  flags1;
    u8  flags2;
    u8  base_high;
    u32 base_upper;
    u32 reserved;
};

struct PACKED Tss {
    u32 reserved0;
    u64 rsp0, rsp1, rsp2;
    u64 reserved1;
    u64 ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
};

static_assert(sizeof(Tss) == 104, "TSS layout must match the x86_64 spec exactly");

constexpr u16 KCODE_SEL = 0x08;
constexpr u16 KDATA_SEL = 0x10;
constexpr u16 TSS_SEL   = 0x28;

constexpr int IST_DOUBLE_FAULT = 1;
constexpr int IST_NMI          = 2;

void init(u64 double_fault_stack_top, u64 nmi_stack_top);

}
