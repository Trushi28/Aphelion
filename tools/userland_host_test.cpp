#include <cosmos/types.hpp>
#include <cosmos/userabi.hpp>
#include <cosmos/usercopy.hpp>
#include <cosmos/usergs.hpp>
#include <cosmos/ringabort.hpp>
#include <stdio.h>

static int g_failures = 0;

static void expect(bool cond, const char* what) {
    printf("%-72s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) ++g_failures;
}

struct FakeSpace {
    static constexpr u64 LIMIT = 0x0000800000000000ull;
    static constexpr u64 PAGE = 4096;

    struct Page {
        u64 va;
        bool writable;
        u8 bytes[PAGE];
    };

    static constexpr u32 MAX_PAGES = 8;
    Page* pages[MAX_PAGES] = {};
    u32 count = 0;

    ~FakeSpace() { for (u32 i = 0; i < count; ++i) delete pages[i]; }

    Page* add(u64 va, bool writable) {
        Page* p = new Page{ va, writable, {} };
        pages[count++] = p;
        return p;
    }

    bool need_write = false;

    usercopy::Window window(u64 uva) {
        for (u32 i = 0; i < count; ++i) {
            Page* p = pages[i];
            if (uva >= p->va && uva < p->va + PAGE) {
                if (need_write && !p->writable) return { nullptr, 0 };
                return { p->bytes + (uva - p->va), p->va + PAGE - uva };
            }
        }
        return { nullptr, 0 };
    }
};

static void range_tests() {
    constexpr u64 L = FakeSpace::LIMIT;
    expect(usercopy::range_ok(0, 0, L), "range: an empty copy at zero is fine");
    expect(usercopy::range_ok(L, 0, L), "range: an empty copy exactly at the limit is fine");
    expect(!usercopy::range_ok(L + 1, 0, L), "range: starting past the limit is refused");
    expect(usercopy::range_ok(L - 8, 8, L), "range: ending exactly at the limit is fine");
    expect(!usercopy::range_ok(L - 8, 9, L), "range: one byte over the limit is refused");
    expect(!usercopy::range_ok(~0ull - 7, 16, L), "range: wrap-around is refused");
    expect(!usercopy::range_ok(0x1000, ~0ull, L), "range: an enormous length is refused");
    expect(!usercopy::range_ok(0xFFFF800000000000ull, 8, L), "range: a kernel-half pointer is refused");
}

static void copy_tests() {
    FakeSpace s;
    auto* a = s.add(0x400000, true);
    auto* b = s.add(0x401000, true);
    auto* c = s.add(0x402000, true);
    s.add(0x403000, false);
    for (u64 i = 0; i < 4096; ++i) { a->bytes[i] = static_cast<u8>(i); b->bytes[i] = static_cast<u8>(i + 7); }

    u8 out[64];
    expect(usercopy::copy_in(s, 0x400010, out, 16) && out[0] == 0x10 && out[15] == 0x1F, "copy_in: inside one page");
    expect(usercopy::copy_in(s, 0x400FF8, out, 16) && out[7] == static_cast<u8>(0xFF) && out[8] == 7,
           "copy_in: straddling two mapped pages");
    expect(!usercopy::copy_in(s, 0x403FF8, out, 16), "copy_in: running into an unmapped page fails");
    expect(!usercopy::copy_in(s, 0x404000, out, 1), "copy_in: an unmapped start fails");
    expect(usercopy::copy_in(s, 0x1234, out, 0), "copy_in: zero length never touches memory");
    expect(!usercopy::copy_in(s, 0xFFFF800000000000ull, out, 8), "copy_in: kernel pointer refused");
    expect(!usercopy::copy_in(s, FakeSpace::LIMIT - 4, out, 8), "copy_in: straddling the user limit refused");

    s.need_write = true;
    u8 payload[32];
    for (u32 i = 0; i < 32; ++i) payload[i] = static_cast<u8>(0xA0 + i);
    expect(usercopy::copy_out(s, 0x400FF0, payload, 32) && a->bytes[0xFF0] == 0xA0 && b->bytes[0xF] == 0xBF,
           "copy_out: writing across two writable pages");

    u8 more[32];
    __builtin_memset(more, 0x55, sizeof(more));
    expect(!usercopy::copy_out(s, 0x402FF0, more, 32), "copy_out: running into a read-only page fails");
    bool clean = true;
    for (u32 i = 0xFF0; i < 4096; ++i) if (c->bytes[i] != 0) clean = false;
    expect(clean, "copy_out: a refused write leaves the writable part untouched");

    FakeSpace t;
    auto* w = t.add(0x500000, true);
    t.add(0x501000, false);
    t.need_write = true;
    u8 fill[8192];
    __builtin_memset(fill, 0x77, sizeof(fill));
    bool refused = !usercopy::copy_out(t, 0x500800, fill, 4096);
    bool untouched = true;
    for (u32 i = 0; i < 4096; ++i) if (w->bytes[i] != 0) untouched = false;
    expect(refused && untouched, "copy_out: validates every page before writing the first byte");
}

static void abi_tests() {
    static_assert(userabi::SYS_COUNT == 6, "the native table has six entries in this series");
    static_assert(userabi::SYS_SET_GS_BASE == 4 && userabi::SYS_GET_GS_BASE == 5, "the GS base calls keep their numbers");
    static_assert(userabi::USER_LIMIT == 0x0000800000000000ull, "the user limit is the bottom of the canonical hole");
    static_assert(userabi::is_fault_status(userabi::fault_status(14)), "a fault status is recognised");
    static_assert(!userabi::is_fault_status(0) && !userabi::is_fault_status(42) && !userabi::is_fault_status(-1),
                  "ordinary exit statuses are not mistaken for faults");
    static_assert(userabi::fault_vector(userabi::fault_status(13)) == 13, "the vector round-trips");
    expect(userabi::fault_status(14) == -270, "fault status for #PF is -270");
    expect(userabi::fault_status(0) == -256, "fault status for #DE is -256");
    expect(userabi::fault_status(13) == -269, "fault status for #GP is -269");
    expect(userabi::STACK_TOP % 4096 == 0 && userabi::CODE_BASE % 4096 == 0 && userabi::DATA_BASE % 4096 == 0,
           "the fixed user layout is page aligned");
    expect(userabi::STACK_TOP < 0x0000800000000000ull, "the user stack sits below the canonical hole");
    expect(userabi::CODE_BASE + userabi::MAX_CODE_PAGES * 4096ull <= userabi::DATA_BASE, "code and data regions do not overlap");
    expect((userabi::SYSCALL_FMASK & (1ull << 9)) && (userabi::SYSCALL_FMASK & (1ull << 10)) && (userabi::SYSCALL_FMASK & (1ull << 18)),
           "SFMASK clears IF, DF and AC on every syscall");
    expect((userabi::SYSRET_RFLAGS_KEEP & ((1ull << 9) | (1ull << 8) | (1ull << 14) | (1ull << 18) | (3ull << 12))) == 0,
           "sysret never carries IF, TF, NT, AC or IOPL back from user-controlled r11");
    expect(userabi::SELECTOR_USER_DATA == 0x1B && userabi::SELECTOR_USER_CODE == 0x23,
           "user selectors match the GDT order used by SYSRET");
}

struct FakeMsr {
    u64 value = 0;
    u32 reads = 0;
    u32 writes = 0;
    u64 read() { ++reads; return value; }
    void write(u64 v) { ++writes; value = v; }
};

static void gs_base_tests() {
    constexpr u64 L = userabi::USER_LIMIT;
    expect(usergs::base_ok(0, L), "gs base: zero is accepted");
    expect(usergs::base_ok(L - 1, L), "gs base: the last canonical user address is accepted");
    expect(!usergs::base_ok(L, L), "gs base: the first address of the canonical hole is refused");
    expect(!usergs::base_ok(0xFFFF800000000000ull, L), "gs base: a kernel-half address is refused");
    expect(!usergs::base_ok(~0ull, L), "gs base: all ones is refused");

    {
        FakeMsr m;
        u64 slot = 111;
        usergs::switch_to(m, false, &slot, false, 0);
        expect(m.reads == 0 && m.writes == 0 && slot == 111, "gs switch: kernel to kernel never touches the MSR");
    }
    {
        FakeMsr m;
        u64 slot = 0;
        usergs::switch_to(m, false, &slot, true, 0x7000);
        expect(m.value == 0x7000 && m.reads == 0 && m.writes == 1, "gs switch: kernel to user loads the user value without reading");
    }
    {
        FakeMsr m;
        m.value = 0x5000;
        u64 slot = 0;
        usergs::switch_to(m, true, &slot, false, 0xDEAD);
        expect(slot == 0x5000 && m.value == 0, "gs switch: user to kernel saves the live value and clears the MSR");
    }
    {
        FakeMsr m;
        m.value = 0;
        u64 slot = 9;
        usergs::switch_to(m, true, &slot, false, 0);
        expect(slot == 0 && m.writes == 0, "gs switch: user to kernel with a zero base skips the write");
    }
    {
        FakeMsr m;
        m.value = 0x5000;
        u64 slot = 0;
        usergs::switch_to(m, true, &slot, true, 0x5000);
        expect(slot == 0x5000 && m.reads == 1 && m.writes == 0, "gs switch: user to user with an equal base skips the write");
    }
    {
        FakeMsr m;
        m.value = 0x5000;
        u64 slot = 0;
        usergs::switch_to(m, true, &slot, true, 0x6000);
        expect(slot == 0x5000 && m.value == 0x6000 && m.writes == 1, "gs switch: user to user saves one base and loads the other");
    }
    {
        FakeMsr m;
        m.value = 0x5000;
        u64 slot = 0;
        usergs::switch_to(m, false, &slot, true, 0);
        expect(m.value == 0 && m.writes == 1, "gs switch: a new user thread never inherits a stale base from a kernel thread's MSR state");
    }
}

struct ModelThread {
    bool user;
    u64 slot;
    u64 expected;
    int on_core;
};

static u64 next_random(u64& s) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}

static void gs_model_test() {
    constexpr u32 CORES = 4;
    constexpr u32 THREADS = 12;
    constexpr u32 STEPS = 400000;
    FakeMsr msr[CORES];
    ModelThread t[THREADS];
    int running[CORES];
    for (u32 i = 0; i < THREADS; ++i) t[i] = { (i % 3) != 0, 0, 0, -1 };
    for (u32 c = 0; c < CORES; ++c) running[c] = -1;

    u64 rng = 0x9E3779B97F4A7C15ull;
    bool leaked = false;
    bool lost = false;
    bool kernel_dirty = false;
    u64 switches = 0;

    for (u32 step = 0; step < STEPS; ++step) {
        u32 c = static_cast<u32>(next_random(rng) % CORES);
        u32 pick = static_cast<u32>(next_random(rng) % THREADS);
        if (t[pick].on_core >= 0) continue;

        int old = running[c];
        bool old_user = old >= 0 && t[old].user;
        u64 dummy = 0;
        u64* old_slot = old >= 0 ? &t[old].slot : &dummy;
        usergs::switch_to(msr[c], old_user, old_slot, t[pick].user, t[pick].slot);
        if (old >= 0) {
            t[old].on_core = -1;
            if (old_user && t[old].slot != t[old].expected) lost = true;
        }
        t[pick].on_core = static_cast<int>(c);
        running[c] = static_cast<int>(pick);
        ++switches;

        u64 want = t[pick].user ? t[pick].expected : 0;
        if (msr[c].value != want) {
            if (t[pick].user) leaked = true;
            else kernel_dirty = true;
        }

        if (t[pick].user && (next_random(rng) & 3) == 0) {
            u64 v = next_random(rng) & (userabi::USER_LIMIT - 1);
            msr[c].write(v);
            t[pick].expected = v;
        }
    }
    expect(switches > STEPS / 4, "gs model: the randomised schedule performed a meaningful number of switches");
    expect(!leaked, "gs model: every user thread always runs with its own base after any switch");
    expect(!kernel_dirty, "gs model: a kernel thread never runs with a user base left in the MSR");
    expect(!lost, "gs model: a base set by a thread is saved exactly when it is switched out");
}

using ringabort::Action;

static ringabort::Mce base_mce() {
    ringabort::Mce m{};
    m.mcg_status = ringabort::MCG_MCIP | ringabort::MCG_EIPV | ringabort::MCG_RIPV;
    m.banks = 4;
    m.phys_mask = ringabort::phys_mask(48);
    return m;
}

static constexpr u64 UC_OWNED = ringabort::STATUS_VAL | ringabort::STATUS_UC | ringabort::STATUS_ADDRV;

static void abort_policy_tests() {
    static_assert(ringabort::on_nmi(true) == Action::Resume, "an NMI in ring 3 is not the process's fault");
    static_assert(ringabort::on_nmi(false) == Action::Panic, "an unexpected NMI in ring 0 is still fatal");
    static_assert(ringabort::on_double_fault(true) == Action::Kill, "a double fault in ring 3 ends the process");
    static_assert(ringabort::on_double_fault(false) == Action::Panic, "a double fault in ring 0 is fatal");
    static_assert(ringabort::nmi_latch_assert(0xC1, ringabort::NMI_CLEAR_SERR) == 0x05,
                  "asserting the SERR clear bit keeps only the low control bits and adds the clear bit");
    static_assert(ringabort::nmi_latch_release(0xC5, ringabort::NMI_CLEAR_SERR) == 0x01,
                  "releasing the SERR clear bit leaves the other control bits as read");
    static_assert((ringabort::nmi_latch_assert(0xCD, ringabort::NMI_CLEAR_IOCHK) & ringabort::NMI_CLEAR_IOCHK) != 0,
                  "the IOCHK latch is cleared by raising its disable bit");
    static_assert((ringabort::nmi_latch_release(0xCF, ringabort::NMI_CLEAR_SERR) & ringabort::NMI_CLEAR_SERR) == 0,
                  "releasing drops the SERR disable bit again");
    static_assert((ringabort::nmi_latch_assert(0xFF, ringabort::NMI_CLEAR_SERR) & (ringabort::NMI_SERR | ringabort::NMI_IOCHK)) == 0,
                  "the status bits are never written back");
    expect(ringabort::phys_mask(48) == 0x0000FFFFFFFFF000ull, "phys mask: 48 bits keeps bits 12..47");
    expect(ringabort::phys_mask(52) == 0x000FFFFFFFFFF000ull, "phys mask: 52 bits keeps bits 12..51");
    expect(ringabort::phys_mask(64) == ringabort::phys_mask(52), "phys mask: an absurd width is clamped to 52");
    expect(ringabort::phys_mask(0) == ringabort::phys_mask(32), "phys mask: a zero width falls back to 32");

    u64 owned[2] = { 0x12345000ull, 0x23456000ull };
    auto owns = [&](u64 f) { return f == owned[0] || f == owned[1]; };

    {
        ringabort::Mce m = base_mce();
        m.status[1] = UC_OWNED;
        m.addr[1] = 0x12345678ull;
        auto v = ringabort::judge_machine_check(true, m, owns);
        expect(v.action == Action::Kill && v.poisoned == 1 && v.frames[0] == 0x12345000ull,
               "mce: an uncorrectable error in the process's own frame kills it and names the frame");
    }
    {
        ringabort::Mce m = base_mce();
        m.status[1] = UC_OWNED;
        m.addr[1] = 0x12345678ull;
        expect(ringabort::judge_machine_check(false, m, owns).action == Action::Panic, "mce: the same error seen from ring 0 is fatal");
    }
    {
        ringabort::Mce m = base_mce();
        m.status[0] = UC_OWNED;
        m.addr[0] = 0x12345000ull;
        m.mcg_status &= ~ringabort::MCG_MCIP;
        expect(ringabort::judge_machine_check(true, m, owns).action == Action::Panic, "mce: without MCIP the state is not trusted");
    }
    {
        ringabort::Mce m = base_mce();
        m.status[0] = UC_OWNED;
        m.addr[0] = 0x12345000ull;
        m.mcg_status &= ~ringabort::MCG_EIPV;
        expect(ringabort::judge_machine_check(true, m, owns).action == Action::Panic,
               "mce: an error not tied to the interrupted instruction is not blamed on the process");
    }
    {
        ringabort::Mce m = base_mce();
        m.mcg_status &= ~ringabort::MCG_RIPV;
        m.status[0] = UC_OWNED;
        m.addr[0] = 0x12345000ull;
        expect(ringabort::judge_machine_check(true, m, owns).action == Action::Kill,
               "mce: a non-restartable error is fine because the process is being killed anyway");
    }
    {
        ringabort::Mce m = base_mce();
        m.status[0] = UC_OWNED | ringabort::STATUS_PCC;
        m.addr[0] = 0x12345000ull;
        expect(ringabort::judge_machine_check(true, m, owns).action == Action::Panic, "mce: corrupted processor context is fatal");
    }
    {
        ringabort::Mce m = base_mce();
        m.status[0] = ringabort::STATUS_VAL | ringabort::STATUS_PCC;
        m.status[1] = UC_OWNED;
        m.addr[1] = 0x12345000ull;
        expect(ringabort::judge_machine_check(true, m, owns).action == Action::Panic,
               "mce: processor-context corruption in any valid bank is fatal, even a corrected one");
    }
    {
        ringabort::Mce m = base_mce();
        m.status[0] = ringabort::STATUS_PCC;
        m.status[1] = UC_OWNED;
        m.addr[1] = 0x12345000ull;
        expect(ringabort::judge_machine_check(true, m, owns).action == Action::Kill, "mce: a bank that is not valid is ignored entirely");
    }
    {
        ringabort::Mce m = base_mce();
        m.status[0] = ringabort::STATUS_VAL | ringabort::STATUS_UC;
        expect(ringabort::judge_machine_check(true, m, owns).action == Action::Panic,
               "mce: an uncorrectable error with no address cannot be tied to the process");
    }
    {
        ringabort::Mce m = base_mce();
        m.status[0] = ringabort::STATUS_VAL | ringabort::STATUS_UC;
        m.addr[0] = 0x12345000ull;
        expect(ringabort::judge_machine_check(true, m, owns).action == Action::Panic,
               "mce: a leftover address whose valid bit is clear is never trusted, even when it matches an owned frame");
    }
    {
        ringabort::Mce m = base_mce();
        m.status[0] = UC_OWNED | ringabort::STATUS_OVER;
        m.addr[0] = 0x12345000ull;
        expect(ringabort::judge_machine_check(true, m, owns).action == Action::Panic, "mce: an overflowed uncorrectable error lost information");
    }
    {
        ringabort::Mce m = base_mce();
        m.status[0] = UC_OWNED;
        m.addr[0] = 0x99999000ull;
        expect(ringabort::judge_machine_check(true, m, owns).action == Action::Panic, "mce: a frame the process does not own is fatal");
    }
    {
        ringabort::Mce m = base_mce();
        m.status[0] = UC_OWNED;
        m.addr[0] = 0x12345000ull;
        m.status[1] = UC_OWNED;
        m.addr[1] = 0x99999000ull;
        expect(ringabort::judge_machine_check(true, m, owns).action == Action::Panic,
               "mce: one foreign frame among owned ones makes the whole event fatal");
    }
    {
        ringabort::Mce m = base_mce();
        m.status[0] = ringabort::STATUS_VAL;
        m.status[1] = ringabort::STATUS_VAL | ringabort::STATUS_ADDRV;
        m.addr[1] = 0x12345000ull;
        expect(ringabort::judge_machine_check(true, m, owns).action == Action::Panic,
               "mce: only corrected errors with MCIP set is an inconsistent state and is fatal");
    }
    {
        ringabort::Mce m = base_mce();
        m.status[0] = ringabort::STATUS_VAL | ringabort::STATUS_ADDRV;
        m.addr[0] = 0x99999000ull;
        m.status[1] = UC_OWNED;
        m.addr[1] = 0x23456000ull;
        auto v = ringabort::judge_machine_check(true, m, owns);
        expect(v.action == Action::Kill && v.poisoned == 1 && v.frames[0] == 0x23456000ull,
               "mce: a corrected error elsewhere neither blocks the kill nor poisons its frame");
    }
    {
        ringabort::Mce m = base_mce();
        m.status[0] = UC_OWNED;
        m.addr[0] = 0x12345000ull;
        m.status[1] = UC_OWNED;
        m.addr[1] = 0x12345FF8ull;
        auto v = ringabort::judge_machine_check(true, m, owns);
        expect(v.action == Action::Kill && v.poisoned == 1, "mce: two errors in one frame poison it once");
    }
    {
        ringabort::Mce m = base_mce();
        m.status[0] = UC_OWNED;
        m.addr[0] = 0x12345000ull;
        m.status[1] = UC_OWNED;
        m.addr[1] = 0x23456000ull;
        auto v = ringabort::judge_machine_check(true, m, owns);
        expect(v.action == Action::Kill && v.poisoned == 2 && v.frames[0] == 0x12345000ull && v.frames[1] == 0x23456000ull,
               "mce: two different owned frames are both poisoned, in bank order");
    }
    {
        ringabort::Mce m = base_mce();
        m.banks = 5;
        auto many = [&](u64) { return true; };
        for (u32 i = 0; i < 5; ++i) {
            m.status[i] = UC_OWNED;
            m.addr[i] = 0x10000000ull + i * 0x1000;
        }
        expect(ringabort::judge_machine_check(true, m, many).action == Action::Panic,
               "mce: more distinct poisoned frames than the process record holds is fatal");
    }
    {
        ringabort::Mce m = base_mce();
        m.addr[0] = 0x12345000ull | (0x3Full << 56);
        m.status[0] = UC_OWNED;
        auto v = ringabort::judge_machine_check(true, m, owns);
        expect(v.action == Action::Kill && v.frames[0] == 0x12345000ull,
               "mce: bits above the physical width (the LSB field on AMD) are masked off before matching");
    }
    {
        ringabort::Mce m = base_mce();
        m.banks = 200;
        m.status[31] = UC_OWNED;
        m.addr[31] = 0x23456000ull;
        auto v = ringabort::judge_machine_check(true, m, owns);
        expect(v.action == Action::Kill && v.frames[0] == 0x23456000ull, "mce: a bank count beyond the array is clamped, never read past");
    }
    {
        ringabort::Mce m = base_mce();
        m.banks = 0;
        expect(ringabort::judge_machine_check(true, m, owns).action == Action::Panic, "mce: no banks to read means nothing to blame");
    }
}

int main() {
    range_tests();
    copy_tests();
    abi_tests();
    gs_base_tests();
    gs_model_test();
    abort_policy_tests();
    printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
