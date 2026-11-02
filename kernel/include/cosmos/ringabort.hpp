#pragma once
#include <cosmos/types.hpp>

namespace ringabort {

enum class Action : u8 { Resume, Kill, Panic };

constexpr u64 MCG_RIPV = 1ull << 0;
constexpr u64 MCG_EIPV = 1ull << 1;
constexpr u64 MCG_MCIP = 1ull << 2;

constexpr u64 STATUS_PCC = 1ull << 57;
constexpr u64 STATUS_ADDRV = 1ull << 58;
constexpr u64 STATUS_UC = 1ull << 61;
constexpr u64 STATUS_OVER = 1ull << 62;
constexpr u64 STATUS_VAL = 1ull << 63;

constexpr u32 MSR_MCG_CAP = 0x179;
constexpr u32 MSR_MCG_STATUS = 0x17A;
constexpr u32 MSR_MC0_STATUS = 0x401;
constexpr u32 MSR_MC0_ADDR = 0x402;
constexpr u32 MSR_BANK_STRIDE = 4;

constexpr u32 MAX_BANKS = 32;
constexpr u32 MAX_POISON = 4;
constexpr u64 FRAME_MASK = ~0xFFFull;

constexpr u16 NMI_PORT = 0x61;
constexpr u8 NMI_SERR = 0x80;
constexpr u8 NMI_IOCHK = 0x40;
constexpr u8 NMI_KEEP = 0x0F;
constexpr u8 NMI_CLEAR_SERR = 0x04;
constexpr u8 NMI_CLEAR_IOCHK = 0x08;

constexpr Action on_nmi(bool from_user) { return from_user ? Action::Resume : Action::Panic; }
constexpr Action on_double_fault(bool from_user) { return from_user ? Action::Kill : Action::Panic; }

constexpr u8 nmi_latch_assert(u8 reason, u8 clear_bit) { return static_cast<u8>((reason & NMI_KEEP) | clear_bit); }
constexpr u8 nmi_latch_release(u8 reason, u8 clear_bit) { return static_cast<u8>((reason & NMI_KEEP) & ~clear_bit); }

constexpr u64 phys_mask(u32 phys_bits) {
    u32 bits = phys_bits < 32 ? 32 : (phys_bits > 52 ? 52 : phys_bits);
    return ((1ull << bits) - 1) & FRAME_MASK;
}

struct Mce {
    u64 mcg_status;
    u32 banks;
    u64 phys_mask;
    u64 status[MAX_BANKS];
    u64 addr[MAX_BANKS];
};

struct Verdict {
    Action action;
    u32 poisoned;
    u64 frames[MAX_POISON];
};

template <typename Owns>
constexpr Verdict judge_machine_check(bool from_user, const Mce& m, Owns&& owns) {
    Verdict refuse{};
    refuse.action = Action::Panic;
    if (!from_user) return refuse;
    if (!(m.mcg_status & MCG_MCIP)) return refuse;
    if (!(m.mcg_status & MCG_EIPV)) return refuse;

    Verdict out{};
    u32 n = m.banks < MAX_BANKS ? m.banks : MAX_BANKS;
    bool saw_uc = false;
    for (u32 i = 0; i < n; ++i) {
        u64 s = m.status[i];
        if (!(s & STATUS_VAL)) continue;
        if (s & STATUS_PCC) return refuse;
        if (!(s & STATUS_UC)) continue;
        if (s & STATUS_OVER) return refuse;
        if (!(s & STATUS_ADDRV)) return refuse;
        saw_uc = true;
        u64 frame = m.addr[i] & m.phys_mask;
        if (!owns(frame)) return refuse;
        bool known = false;
        for (u32 k = 0; k < out.poisoned; ++k) {
            if (out.frames[k] == frame) known = true;
        }
        if (known) continue;
        if (out.poisoned == MAX_POISON) return refuse;
        out.frames[out.poisoned++] = frame;
    }
    if (!saw_uc) return refuse;
    out.action = Action::Kill;
    return out;
}

}
