#include <cosmos/types.hpp>
#include <cosmos/userabi.hpp>
#include <cosmos/usercopy.hpp>
#include <cosmos/usergs.hpp>
#include <cosmos/ringabort.hpp>
#include <cosmos/entrycheck.hpp>
#include <cosmos/procgroup.hpp>
#include <cosmos/handleabi.hpp>
#include <cosmos/vmregion.hpp>
#include <cosmos/apx.hpp>
#include <cosmos/fileabi.hpp>
#include <stdio.h>
#include <pthread.h>

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
    static_assert(userabi::SYS_COUNT == 33, "the native table has thirty-three entries in this series");
    static_assert(userabi::SYS_THREAD_CREATE == 8 && userabi::SYS_THREAD_EXIT == 9 && userabi::SYS_THREAD_JOIN == 10, "the thread calls follow the FS ones");
    static_assert(!userabi::is_fault_status(userabi::STATUS_KILLED), "a kill is not mistaken for a fault");
    static_assert(userabi::MAX_LIVE_THREADS <= userabi::MAX_THREADS_TOTAL && userabi::MAX_THREADS_TOTAL < procgroup::LIVE_MAX, "the thread limits fit the group word");
    static_assert(userabi::SYS_SET_GS_BASE == 4 && userabi::SYS_GET_GS_BASE == 5, "the GS base calls keep their numbers");
    static_assert(userabi::SYS_SET_FS_BASE == 6 && userabi::SYS_GET_FS_BASE == 7, "the FS base calls follow the GS ones");
    static_assert(usergs::MSR_FS_BASE == 0xC0000100 && usergs::MSR_KERNEL_GS_BASE == 0xC0000102, "the two base MSRs are the architectural ones");
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

static void fs_gs_pair_model_test() {
    constexpr u32 CORES = 4;
    constexpr u32 THREADS = 12;
    constexpr u32 STEPS = 300000;
    struct Pair { FakeMsr gs; FakeMsr fs; };
    struct Thr { bool user; u64 gs_slot; u64 fs_slot; u64 gs_want; u64 fs_want; int on_core; };
    Pair msr[CORES];
    Thr t[THREADS];
    int running[CORES];
    for (u32 i = 0; i < THREADS; ++i) t[i] = { (i % 3) != 0, 0, 0, 0, 0, -1 };
    for (u32 c = 0; c < CORES; ++c) running[c] = -1;
    u64 rng = 0xD1B54A32D192ED03ull;
    bool crossed = false, leaked = false, dirty = false;
    for (u32 step = 0; step < STEPS; ++step) {
        u32 c = static_cast<u32>(next_random(rng) % CORES);
        u32 pick = static_cast<u32>(next_random(rng) % THREADS);
        if (t[pick].on_core >= 0) continue;
        int old = running[c];
        bool old_user = old >= 0 && t[old].user;
        u64 dg = 0, df = 0;
        usergs::switch_to(msr[c].gs, old_user, old >= 0 ? &t[old].gs_slot : &dg, t[pick].user, t[pick].gs_slot);
        usergs::switch_to(msr[c].fs, old_user, old >= 0 ? &t[old].fs_slot : &df, t[pick].user, t[pick].fs_slot);
        if (old >= 0) t[old].on_core = -1;
        t[pick].on_core = static_cast<int>(c);
        running[c] = static_cast<int>(pick);
        if (t[pick].user) {
            if (msr[c].gs.value != t[pick].gs_want || msr[c].fs.value != t[pick].fs_want) leaked = true;
            if (t[pick].gs_want == t[pick].fs_want && t[pick].gs_want != 0) crossed = true;
        } else if (msr[c].gs.value != 0 || msr[c].fs.value != 0) {
            dirty = true;
        }
        if (t[pick].user && (next_random(rng) & 3) == 0) {
            u64 g = (next_random(rng) & (userabi::USER_LIMIT - 1)) | 1;
            u64 f = (next_random(rng) & (userabi::USER_LIMIT - 1)) | 2;
            if (next_random(rng) & 1) { msr[c].gs.write(g); t[pick].gs_want = g; }
            else { msr[c].fs.write(f); t[pick].fs_want = f; }
        }
    }
    expect(!leaked, "fs/gs model: each user thread always runs with both of its own bases, whichever one changed last");
    expect(!dirty, "fs/gs model: a kernel thread never runs with either user base loaded");
    expect(!crossed, "fs/gs model: writing one base never changes the other");
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

static void entry_fence_tests() {
    using entrycheck::Verdict;
    const u8 good[] = { 0x48, 0x89, 0xE7, 0x0F, 0x01, 0xF8, 0xBB, 0x01, 0, 0, 0, 0x0F, 0xAE, 0xE8, 0x90 };
    const u8 adjacent[] = { 0x0F, 0x01, 0xF8, 0x0F, 0xAE, 0xE8 };
    const u8 bare[] = { 0x90, 0x0F, 0x01, 0xF8, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    const u8 late[] = { 0x0F, 0x01, 0xF8, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x0F, 0xAE, 0xE8 };
    const u8 before[] = { 0x0F, 0xAE, 0xE8, 0x0F, 0x01, 0xF8, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    const u8 none[] = { 0x90, 0x90, 0x90, 0x90 };
    const u8 cut[] = { 0x90, 0x0F, 0x01, 0xF8, 0x0F };
    const u8 second[] = { 0x0F, 0x01, 0xF8, 0x0F, 0xAE, 0xE8, 0x90, 0x0F, 0x01, 0xF8, 0x90 };
    expect(entrycheck::first_swapgs(good, sizeof(good)) == Verdict::Fenced, "entry fence: an lfence a few bytes after swapgs is found");
    expect(entrycheck::first_swapgs(adjacent, sizeof(adjacent)) == Verdict::Fenced, "entry fence: an lfence right after swapgs is found");
    expect(entrycheck::first_swapgs(bare, sizeof(bare)) == Verdict::Unfenced, "entry fence: a swapgs with no lfence is reported");
    expect(entrycheck::first_swapgs(late, sizeof(late)) == Verdict::Unfenced, "entry fence: an lfence beyond the reach does not count");
    expect(entrycheck::first_swapgs(before, sizeof(before)) == Verdict::Unfenced, "entry fence: an lfence before the swapgs does not count");
    expect(entrycheck::first_swapgs(none, sizeof(none)) == Verdict::NoSwapgs, "entry fence: code without swapgs is reported as such");
    expect(entrycheck::first_swapgs(cut, sizeof(cut)) == Verdict::Unfenced, "entry fence: a truncated window never reads past it");
    expect(entrycheck::first_swapgs(second, sizeof(second)) == Verdict::Fenced, "entry fence: only the first swapgs is judged (the exit swapgs is not fenced)");
    expect(entrycheck::first_swapgs(good, 2) == Verdict::NoSwapgs, "entry fence: a window shorter than an instruction is safe");
}

static void procgroup_tests() {
    using namespace procgroup;
    u64 w = 0;
    expect(finished(w), "group: a zeroed word is not a running group");
    expect(!try_enter(&w, 8), "group: nothing can join a group that never started");
    expect(!request_exit(&w, 5), "group: a group that never started cannot be told to exit");
    start(&w);
    expect(live(load(&w)) == 1 && !dying(load(&w)) && !finished(load(&w)), "group: a started group has one live thread");
    expect(try_enter(&w, 3) && try_enter(&w, 3), "group: threads join up to the limit");
    expect(!try_enter(&w, 3), "group: the limit is enforced");
    expect(live(load(&w)) == 3, "group: three threads are live");
    Leave l = leave(&w, 7);
    expect(!l.last && live(load(&w)) == 2, "group: a thread leaving while others remain is not the last");
    expect(request_exit(&w, -512), "group: the first exit request wins");
    expect(!request_exit(&w, 9), "group: a second exit request is refused");
    expect(status(load(&w)) == -512 && dying(load(&w)), "group: the winning status is kept");
    expect(!try_enter(&w, 8), "group: nothing can join a dying group");
    l = leave(&w, 11);
    expect(!l.last && !finished(load(&w)), "group: a leave in a dying group with others left is not last");
    l = leave(&w, 13);
    expect(l.last && l.status == -512 && finished(load(&w)), "group: the last leaver of a dying group reports the winning status");
    expect(status(load(&w)) == -512, "group: the finished word keeps that status");
    expect(!request_exit(&w, 1), "group: a finished group refuses a late kill");
    expect(!try_enter(&w, 8), "group: a finished group refuses new threads");
    expect(!leave(&w, 3).last, "group: leaving a finished group changes nothing");

    start(&w);
    try_enter(&w, 4);
    leave(&w, 5);
    l = leave(&w, 42);
    expect(l.last && l.status == 42 && !dying(load(&w)), "group: with no exit request the last thread's own status is the result");

    start(&w);
    request_exit(&w, userabi::fault_status(14));
    l = leave(&w, 0);
    expect(l.last && l.status == userabi::fault_status(14), "group: a fault status survives the round trip");
    expect(pack(2, true, -1) == ((2ull << 32) | (1ull << 48) | 0xFFFFFFFFull), "group: the word layout is fixed");
    expect(status(pack(1, false, -1)) == -1 && live(pack(1, false, -1)) == 1, "group: a negative status does not leak into the count");

    constexpr u64 L = 0x0000800000000000ull;
    expect(entry_ok(0x400000, 0x7FFFFFF00000, L), "thread args: ordinary values are accepted");
    expect(!entry_ok(0, 0x1000, L), "thread args: a zero entry is refused");
    expect(!entry_ok(0x400000, 0, L), "thread args: a zero stack is refused");
    expect(!entry_ok(L, 0x1000, L), "thread args: an entry at the canonical hole is refused");
    expect(!entry_ok(0x400000, L, L), "thread args: a stack at the canonical hole is refused");
    expect(!entry_ok(0xFFFF800000000000ull, 0x1000, L), "thread args: a kernel-half entry is refused");
    expect(!entry_ok(0x400000, 0xFFFFFFFFFFFFFFF0ull, L), "thread args: a kernel-half stack is refused");
    expect(entry_ok(L - 1, L - 1, L), "thread args: the last user byte is accepted");
}

static void procgroup_model_test() {
    using namespace procgroup;
    u64 state = 0x243F6A8885A308D3ull;
    auto next = [&]() { state = state * 6364136223846793005ull + 1442695040888963407ull; return static_cast<u32>(state >> 33); };
    bool ok = true;
    for (u32 round = 0; round < 2000 && ok; ++round) {
        u64 w = 0;
        start(&w);
        u32 m_live = 1;
        bool m_dying = false, m_done = false;
        i32 m_status = 0;
        const u32 limit = 1 + next() % 6;
        for (u32 step = 0; step < 40 && ok; ++step) {
            switch (next() % 3) {
            case 0: {
                bool want = !m_done && !m_dying && m_live < limit;
                bool got = try_enter(&w, limit);
                if (want != got) ok = false;
                if (want) ++m_live;
                break;
            }
            case 1: {
                i32 v = static_cast<i32>(next() % 2000) - 1000;
                bool want = !m_done && !m_dying;
                bool got = request_exit(&w, v);
                if (want != got) ok = false;
                if (want) { m_dying = true; m_status = v; }
                break;
            }
            default: {
                i32 own = static_cast<i32>(next() % 2000) - 1000;
                Leave r = leave(&w, own);
                if (m_done) { if (r.last) ok = false; break; }
                --m_live;
                bool want_last = m_live == 0;
                if (want_last) {
                    m_done = true;
                    if (!m_dying) m_status = own;
                    if (!r.last || r.status != m_status) ok = false;
                } else if (r.last) ok = false;
                break;
            }
            }
            u64 cur = load(&w);
            if (live(cur) != m_live || dying(cur) != m_dying || finished(cur) != m_done) ok = false;
            if ((m_dying || m_done) && status(cur) != m_status) ok = false;
        }
    }
    expect(ok, "group model: 2000 random sequences agree with a plain model");
}

struct GroupStress {
    u64 word;
    u32 limit;
    u32 lasts;
    u32 winners;
    i32 winner_value;
    u32 entered;
    u32 left;
};

struct GroupWorker {
    GroupStress* shared;
    u32 seed;
    i32 own;
};

static void* group_worker(void* arg) {
    GroupWorker* me = static_cast<GroupWorker*>(arg);
    GroupStress* g = me->shared;
    u32 state = me->seed;
    auto next = [&]() { state = state * 1664525u + 1013904223u; return state >> 8; };
    for (u32 i = 0; i < 200; ++i) {
        switch (next() % 3) {
        case 0:
            if (procgroup::try_enter(&g->word, g->limit)) {
                __atomic_add_fetch(&g->entered, 1u, __ATOMIC_RELAXED);
                procgroup::Leave l = procgroup::leave(&g->word, 0);
                __atomic_add_fetch(&g->left, 1u, __ATOMIC_RELAXED);
                if (l.last) __atomic_add_fetch(&g->lasts, 1u, __ATOMIC_RELAXED);
            }
            break;
        case 1:
            if (next() % 40 == 0) {
                i32 v = me->own + 1000;
                if (procgroup::request_exit(&g->word, v)) {
                    __atomic_add_fetch(&g->winners, 1u, __ATOMIC_RELAXED);
                    g->winner_value = v;
                }
            }
            break;
        default:
            break;
        }
    }
    procgroup::Leave l = procgroup::leave(&g->word, me->own);
    __atomic_add_fetch(&g->left, 1u, __ATOMIC_RELAXED);
    if (l.last) __atomic_add_fetch(&g->lasts, 1u, __ATOMIC_RELAXED);
    return nullptr;
}

static void procgroup_stress_test() {
    bool ok = true;
    for (u32 round = 0; round < 300 && ok; ++round) {
        constexpr u32 THREADS = 6;
        GroupStress g{};
        g.limit = 8;
        procgroup::start(&g.word);
        GroupWorker workers[THREADS];
        pthread_t ids[THREADS];
        for (u32 i = 0; i < THREADS - 1; ++i) {
            if (!procgroup::try_enter(&g.word, 64)) ok = false;
        }
        for (u32 i = 0; i < THREADS; ++i) {
            workers[i] = { &g, round * 31 + i * 7919 + 1, static_cast<i32>(i + 1) };
            pthread_create(&ids[i], nullptr, &group_worker, &workers[i]);
        }
        for (u32 i = 0; i < THREADS; ++i) pthread_join(ids[i], nullptr);
        u64 final_word = procgroup::load(&g.word);
        if (!procgroup::finished(final_word)) ok = false;
        if (g.lasts != 1) ok = false;
        if (g.entered + THREADS != g.left) ok = false;
        if (g.winners > 1) ok = false;
        if (g.winners == 1 && procgroup::status(final_word) != g.winner_value) ok = false;
        if (g.winners == 0 && (procgroup::status(final_word) < 1 || procgroup::status(final_word) > static_cast<i32>(THREADS))) ok = false;
    }
    expect(ok, "group stress: 300 rounds of 6 racing threads end with one last leaver and a consistent status");
}


struct RefOps {
    int refs[16] = {};
    void retain(void* o) { ++refs[reinterpret_cast<usize>(o)]; }
    void release(void* o) { --refs[reinterpret_cast<usize>(o)]; }
};

struct Cell {
    int obj;
    u32 offset;
    u32 prot;
    u32 max;
    bool operator==(const Cell&) const = default;
};

struct PageModel {
    static constexpr u32 SLOTS = 1024;
    bool present[SLOTS] = {};
    Cell cell[SLOTS] = {};
    u32 total = 0;

    u32 count(u64 page) const { return page < SLOTS && present[page] ? 1 : 0; }
    u32 size() const { return total; }
    void set(u64 page, const Cell& c) {
        if (!present[page]) ++total;
        present[page] = true;
        cell[page] = c;
    }
    void erase(u64 page) {
        if (present[page]) --total;
        present[page] = false;
    }
    Cell* find(u64 page) { return page < SLOTS && present[page] ? &cell[page] : nullptr; }
    bool operator==(const PageModel& o) const {
        if (total != o.total) return false;
        for (u32 i = 0; i < SLOTS; ++i) {
            if (present[i] != o.present[i]) return false;
            if (present[i] && !(cell[i] == o.cell[i])) return false;
        }
        return true;
    }
};

static u64 page_of(u64 va) { return (va - vmregion::WINDOW_BASE) / vmregion::PAGE; }
static u64 va_of(u64 page) { return vmregion::WINDOW_BASE + page * vmregion::PAGE; }

template <u32 N>
static PageModel expand(const vmregion::Map<N>& m) {
    PageModel out;
    for (u32 i = 0; i < m.count(); ++i) {
        const vmregion::Region& r = m.at(i);
        for (u32 k = 0; k < r.pages; ++k)
            out.set(page_of(r.base) + k, Cell{ static_cast<int>(reinterpret_cast<usize>(r.object)), r.offset + k, r.prot, r.max });
    }
    return out;
}

template <u32 N>
static bool refs_match(const vmregion::Map<N>& m, const RefOps& ops) {
    int entries[16] = {};
    for (u32 i = 0; i < m.count(); ++i) ++entries[reinterpret_cast<usize>(m.at(i).object)];
    for (int id = 0; id < 16; ++id)
        if (ops.refs[id] != entries[id]) return false;
    return true;
}

template <u32 N>
static bool fully_merged(const vmregion::Map<N>& m) {
    for (u32 i = 0; i + 1 < m.count(); ++i)
        if (vmregion::mergeable(m.at(i), m.at(i + 1))) return false;
    return true;
}

static u32 rng_next(u64& s) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return static_cast<u32>(s >> 11);
}

static void vmregion_basic_tests() {
    using namespace vmregion;
    expect(check_prot(PROT_READ, PROT_ALL) == Status::Ok, "vmregion: read is a valid protection");
    expect(check_prot(PROT_READ | PROT_WRITE, PROT_ALL) == Status::Ok, "vmregion: read-write is valid");
    expect(check_prot(PROT_READ | PROT_EXEC, PROT_ALL) == Status::Ok, "vmregion: read-execute is valid");
    expect(check_prot(PROT_READ | PROT_WRITE | PROT_EXEC, PROT_ALL) == Status::Denied, "vmregion: write plus execute is denied");
    expect(check_prot(PROT_WRITE, PROT_ALL) == Status::Invalid, "vmregion: write without read is invalid");
    expect(check_prot(0, PROT_ALL) == Status::Invalid, "vmregion: no access is invalid");
    expect(check_prot(PROT_READ | 8, PROT_ALL) == Status::Invalid, "vmregion: unknown bits are invalid");
    expect(check_prot(PROT_READ | PROT_WRITE, PROT_READ) == Status::Denied, "vmregion: beyond the handle's maximum is denied");
    expect(check_prot(PROT_READ, PROT_READ) == Status::Ok, "vmregion: equal to the maximum is fine");

    expect(range_ok(WINDOW_BASE, 1), "vmregion: the first window page is valid");
    expect(!range_ok(WINDOW_BASE - PAGE, 1), "vmregion: below the window is refused");
    expect(!range_ok(WINDOW_END, 1), "vmregion: at the window end is refused");
    expect(range_ok(WINDOW_END - PAGE, 1), "vmregion: the last window page is valid");
    expect(!range_ok(WINDOW_END - PAGE, 2), "vmregion: running past the window end is refused");
    expect(!range_ok(WINDOW_BASE + 1, 1), "vmregion: an unaligned base is refused");
    expect(!range_ok(WINDOW_BASE, 0), "vmregion: zero pages is refused");
    expect(!range_ok(WINDOW_BASE, MAX_MAPPED_PAGES + 1), "vmregion: more than the process limit is refused");
    expect(!range_ok(~0ull & ~(PAGE - 1), 1), "vmregion: the top of the address space is refused");
    expect(!range_ok(WINDOW_BASE, 0xFFFFFFFFull), "vmregion: a huge page count cannot wrap");

    RefOps ops;
    Map<> m;
    u64 at = 0;
    expect(m.find_gap(4, &at) == Status::Ok && at == WINDOW_BASE, "vmregion: an empty map hands out the window base");
    ops.refs[1] = 1;
    expect(m.insert({ WINDOW_BASE, 4, 0, PROT_READ, PROT_ALL, reinterpret_cast<void*>(1) }) == Status::Ok, "vmregion: first insert");
    expect(m.find_gap(2, &at) == Status::Ok && at == WINDOW_BASE + 4 * PAGE, "vmregion: the next gap follows the first region");
    expect(m.check_free(WINDOW_BASE + 3 * PAGE, 1) == Status::Exists, "vmregion: overlapping the last page is refused");
    expect(m.check_free(WINDOW_BASE + 4 * PAGE, 1) == Status::Ok, "vmregion: the page after is free");
    expect(m.check_free(WINDOW_BASE - PAGE, 2) == Status::Invalid, "vmregion: straddling the window start is invalid");
    expect(m.insert({ WINDOW_BASE + 2 * PAGE, 1, 0, PROT_READ, PROT_ALL, reinterpret_cast<void*>(1) }) == Status::Exists, "vmregion: inserting inside is refused");
    expect(m.insert({ WINDOW_BASE + 8 * PAGE, 1, 0, PROT_READ | PROT_WRITE | PROT_EXEC, PROT_ALL, reinterpret_cast<void*>(1) }) == Status::Invalid,
           "vmregion: an invalid protection is refused at insert");
    expect(m.insert({ WINDOW_BASE + 8 * PAGE, 1, 0, PROT_READ | PROT_WRITE, PROT_READ, reinterpret_cast<void*>(1) }) == Status::Invalid,
           "vmregion: a protection above the maximum is refused at insert");
    expect(m.find(WINDOW_BASE + 3 * PAGE + 5) && !m.find(WINDOW_BASE + 4 * PAGE), "vmregion: lookup respects the region end");
    expect(m.unmap(WINDOW_BASE + 8 * PAGE, 1, ops) == Status::NotMapped, "vmregion: unmapping nothing reports not mapped");
    expect(m.unmap(WINDOW_BASE + 3 * PAGE, 2, ops) == Status::NotMapped && m.count() == 1 && m.mapped_pages() == 4,
           "vmregion: a range with a hole is refused and changes nothing");
    expect(m.protect(WINDOW_BASE + 3 * PAGE, 2, PROT_READ, ops) == Status::NotMapped, "vmregion: protecting across a hole is refused");
    expect(m.unmap(WINDOW_BASE + PAGE, 2, ops) == Status::Ok && m.count() == 2 && m.mapped_pages() == 2 && ops.refs[1] == 2,
           "vmregion: punching a hole splits one region in two and takes a reference");
    expect(m.at(0).pages == 1 && m.at(1).base == WINDOW_BASE + 3 * PAGE && m.at(1).offset == 3, "vmregion: the right half keeps its object offset");
    m.clear(ops);
    expect(m.count() == 0 && m.mapped_pages() == 0 && ops.refs[1] == 0, "vmregion: clear releases every reference");
}

template <u32 N>
static void vmregion_model_run(u32 seed_index, u32 steps, u32 span, u32 max_len) {
    using namespace vmregion;
    u64 rng = 0x9E3779B97F4A7C15ull ^ (static_cast<u64>(seed_index) * 0xD1B54A32D192ED03ull);
    RefOps ops;
    Map<N> m;
    PageModel model;
    bool agree = true;
    bool refs_ok = true;
    bool merged_ok = true;
    bool consistent_ok = true;
    u32 limits = 0;
    u32 done[6] = {};
    for (u32 step = 0; step < steps && agree; ++step) {
        u32 op = rng_next(rng) % 10;
        u32 pages = 1 + rng_next(rng) % max_len;
        u64 base_page = rng_next(rng) % span;
        u64 base = va_of(base_page);
        if (op < 4) {
            int obj = 1 + static_cast<int>(rng_next(rng) % 5);
            u32 max = PROT_READ | ((rng_next(rng) & 1) ? PROT_WRITE : 0) | ((rng_next(rng) & 1) ? PROT_EXEC : 0);
            u32 choices[3] = { PROT_READ, PROT_READ | PROT_WRITE, PROT_READ | PROT_EXEC };
            u32 prot = choices[rng_next(rng) % 3];
            u32 offset = rng_next(rng) % 40;
            bool fixed = rng_next(rng) & 1;
            if (!fixed) {
                u64 gap = 0;
                Status g = m.find_gap(pages, &gap);
                u64 expect_page = 0;
                for (;; ++expect_page) {
                    bool free_run = true;
                    for (u32 k = 0; k < pages && free_run; ++k)
                        if (model.count(expect_page + k)) { free_run = false; expect_page += k; }
                    if (free_run) break;
                }
                if (g != Status::Ok || gap != va_of(expect_page)) agree = false;
                base = gap;
            }
            ops.refs[obj]++;
            Region r{ base, pages, offset, prot, max, reinterpret_cast<void*>(static_cast<usize>(obj)) };
            Status want = Status::Ok;
            for (u32 k = 0; k < pages && want == Status::Ok; ++k)
                if (model.count(page_of(base) + k)) want = Status::Exists;
            if (want == Status::Ok && check_prot(prot, max) != Status::Ok) want = Status::Invalid;
            Status got = m.insert(r);
            if (got == Status::Limit) {
                ++limits;
                ops.refs[obj]--;
            } else {
                if (got != want) agree = false;
                if (got == Status::Ok) {
                    for (u32 k = 0; k < pages; ++k) model.set(page_of(base) + k, Cell{ obj, offset + k, prot, max });
                } else {
                    ops.refs[obj]--;
                }
            }
            ++done[0];
        } else if (op < 7) {
            bool whole = true;
            for (u32 k = 0; k < pages; ++k) whole = whole && model.count(base_page + k);
            Status got = m.unmap(base, pages, ops);
            if (got == Status::Limit) {
                ++limits;
            } else {
                if (got != (whole ? Status::Ok : Status::NotMapped)) agree = false;
                if (got == Status::Ok)
                    for (u32 k = 0; k < pages; ++k) model.erase(base_page + k);
            }
            ++done[1];
        } else if (op < 9) {
            u32 choices[5] = { PROT_READ, PROT_READ | PROT_WRITE, PROT_READ | PROT_EXEC, PROT_READ | PROT_WRITE | PROT_EXEC, PROT_WRITE };
            u32 prot = choices[rng_next(rng) % 5];
            bool whole = true;
            bool allowed = true;
            for (u32 k = 0; k < pages; ++k) {
                Cell* c = model.find(base_page + k);
                if (!c) { whole = false; continue; }
                if (prot & ~c->max) allowed = false;
            }
            Status want = check_prot(prot, PROT_ALL);
            if (want == Status::Ok) want = !whole ? Status::NotMapped : (allowed ? Status::Ok : Status::Denied);
            Status got = m.protect(base, pages, prot, ops);
            if (got == Status::Limit) {
                ++limits;
            } else {
                if (got != want) agree = false;
                if (got == Status::Ok) {
                    for (u32 k = 0; k < pages; ++k) model.find(base_page + k)->prot = prot;
                    if (!fully_merged(m)) merged_ok = false;
                }
            }
            ++done[2];
        } else {
            const Region* r = m.find(base);
            Cell* c = model.find(base_page);
            if ((r != nullptr) != (c != nullptr)) agree = false;
            else if (r && static_cast<int>(reinterpret_cast<usize>(r->object)) != c->obj) agree = false;
            ++done[3];
        }
        if (!(expand(m) == model)) agree = false;
        if (!refs_match(m, ops)) refs_ok = false;
        if (!m.consistent()) consistent_ok = false;
        if (m.mapped_pages() != model.size()) agree = false;
    }
    char label[128];
    snprintf(label, sizeof(label), "vmregion: model run %u (%u regions max) agrees page by page", seed_index, N);
    expect(agree, label);
    expect(refs_ok, "vmregion: references always equal the number of entries");
    expect(consistent_ok, "vmregion: entries stay sorted, disjoint and valid");
    expect(merged_ok, "vmregion: protect leaves no mergeable neighbours");
    expect(done[0] && done[1] && done[2], "vmregion: every operation kind ran");
    m.clear(ops);
    bool zero = true;
    for (int id = 0; id < 16; ++id) zero = zero && ops.refs[id] == 0;
    expect(zero, "vmregion: clearing releases everything the run took");
    if (N < 16) expect(limits > 0, "vmregion: a small table hits its limit and stays consistent");
}

static void vmregion_limit_tests() {
    using namespace vmregion;
    RefOps ops;
    Map<4> m;
    void* obj = reinterpret_cast<void*>(1);
    for (u32 i = 0; i < 4; ++i) {
        ops.refs[1]++;
        expect(m.insert({ WINDOW_BASE + i * 2 * PAGE, 1, 0, PROT_READ, PROT_ALL, obj }) == Status::Ok, "vmregion: filling the table");
    }
    ops.refs[1]++;
    expect(m.insert({ WINDOW_BASE + 100 * PAGE, 1, 0, PROT_READ, PROT_ALL, obj }) == Status::Limit, "vmregion: a full table refuses another region");
    ops.refs[1]--;
    Map<4> one;
    RefOps ops2;
    ops2.refs[1] = 1;
    one.insert({ WINDOW_BASE, 10, 0, PROT_READ, PROT_ALL, obj });
    ops2.refs[1] = 1;
    expect(one.protect(WINDOW_BASE + 9 * PAGE, 1, PROT_READ | PROT_WRITE, ops2) == Status::Ok && one.count() == 2, "vmregion: protecting the last page splits off one entry");
    expect(one.protect(WINDOW_BASE + 7 * PAGE, 1, PROT_READ | PROT_WRITE, ops2) == Status::Ok && one.count() == 4, "vmregion: protecting a middle page splits into three");
    expect(one.protect(WINDOW_BASE + 4 * PAGE, 1, PROT_READ | PROT_WRITE, ops2) == Status::Limit && one.count() == 4,
           "vmregion: a split that would exceed the table is refused untouched");
    expect(ops2.refs[1] == 4, "vmregion: references equal entries at the limit");
    one.protect(WINDOW_BASE + 9 * PAGE, 1, PROT_READ, ops2);
    one.protect(WINDOW_BASE + 7 * PAGE, 1, PROT_READ, ops2);
    expect(one.count() == 1 && ops2.refs[1] == 1 && one.at(0).pages == 10, "vmregion: restoring the protection merges the region back");

    Map<2> two;
    RefOps ops3;
    ops3.refs[1] = 1;
    two.insert({ WINDOW_BASE, 10, 0, PROT_READ, PROT_ALL, obj });
    expect(two.protect(WINDOW_BASE + 3 * PAGE, 2, PROT_READ | PROT_WRITE, ops3) == Status::Limit && two.count() == 1 && ops3.refs[1] == 1,
           "vmregion: a middle protect needing two extra entries is refused untouched");
    expect(two.unmap(WINDOW_BASE + 3 * PAGE, 2, ops3) == Status::Ok && two.count() == 2 && ops3.refs[1] == 2,
           "vmregion: a hole needing one extra entry fits");
    expect(two.unmap(WINDOW_BASE + 6 * PAGE, 1, ops3) == Status::Limit && two.count() == 2 && ops3.refs[1] == 2,
           "vmregion: a second hole is refused untouched");
    expect(two.unmap(WINDOW_BASE, 3, ops3) == Status::Ok && two.count() == 1 && ops3.refs[1] == 1, "vmregion: trimming the front needs no extra entry");

    Map<8> adj;
    RefOps ops4;
    ops4.refs[1] = 2;
    adj.insert({ WINDOW_BASE, 2, 0, PROT_READ, PROT_ALL, obj });
    adj.insert({ WINDOW_BASE + 2 * PAGE, 2, 2, PROT_READ, PROT_ALL, obj });
    expect(adj.count() == 2, "vmregion: insert alone does not merge");
    expect(adj.protect(WINDOW_BASE, 4, PROT_READ, ops4) == Status::Ok && adj.count() == 1 && ops4.refs[1] == 1 && adj.at(0).pages == 4,
           "vmregion: a no-op protect merges contiguous neighbours");
    ops4.refs[2] = 1;
    adj.insert({ WINDOW_BASE + 4 * PAGE, 2, 4, PROT_READ, PROT_ALL, reinterpret_cast<void*>(2) });
    ops4.refs[1]++;
    adj.insert({ WINDOW_BASE + 6 * PAGE, 2, 7, PROT_READ, PROT_ALL, obj });
    adj.protect(WINDOW_BASE, 8, PROT_READ, ops4);
    expect(adj.count() == 3, "vmregion: different objects or non-contiguous offsets never merge");
    adj.clear(ops4);
}

static void vmregion_tests() {
    vmregion_basic_tests();
    vmregion_limit_tests();
    for (u32 i = 0; i < 6; ++i) vmregion_model_run<64>(i, 6000, 120, 24);
    for (u32 i = 0; i < 4; ++i) vmregion_model_run<6>(100 + i, 4000, 40, 8);
}

static void handle_abi_tests() {
    using lens::Status;
    static_assert(userabi::SYS_HANDLE_CLOSE == 11 && userabi::SYS_HANDLE_WAIT == 17, "the handle calls follow the thread ones");
    static_assert(userabi::SYS_CHANNEL_CREATE == userabi::SYS_HANDLE_NARROW + 1 && userabi::SYS_CHANNEL_SEND == userabi::SYS_CHANNEL_CREATE + 1 &&
                  userabi::SYS_CHANNEL_RECV == userabi::SYS_CHANNEL_SEND + 1, "the channel calls are consecutive");
    static_assert(sizeof(handleabi::WaitEntry) == 16, "a wait entry is 16 bytes");
    static_assert(handleabi::WAIT_READABLE == 1 && handleabi::WAIT_WRITABLE == 2 && handleabi::WAIT_HANGUP == 4, "wait bits match the Lens readiness bits");

    bool all_mapped = true;
    bool ok_only_zero = true;
    for (i32 v = 0; v <= static_cast<i32>(Status::Cancelled); ++v) {
        i64 e = handleabi::error_of(static_cast<Status>(v));
        if (v == 0 ? e != 0 : e >= 0) ok_only_zero = false;
        if (e < handleabi::ERR_CANCELLED) all_mapped = false;
    }
    expect(ok_only_zero, "errors: Ok maps to zero and every other status to a negative number");
    expect(all_mapped, "errors: every status maps to a defined error code");
    expect(handleabi::error_of(Status::NotReady) == handleabi::ERR_AGAIN, "errors: NotReady is AGAIN");
    expect(handleabi::error_of(Status::Closed) == handleabi::ERR_CLOSED, "errors: Closed is CLOSED");
    expect(handleabi::error_of(Status::Denied) == handleabi::ERR_DENIED, "errors: Denied is DENIED");
    expect(handleabi::error_of(Status::BadLens) == handleabi::ERR_BADHANDLE && handleabi::error_of(Status::StaleLens) == handleabi::ERR_BADHANDLE,
           "errors: bad and stale handles are BADHANDLE");
    expect(handleabi::error_of(Status::TooLarge) == handleabi::ERR_TOOBIG, "errors: TooLarge is TOOBIG");
    expect(handleabi::error_of(Status::TimedOut) == handleabi::ERR_TIMEDOUT, "errors: TimedOut is TIMEDOUT");
    expect(handleabi::error_of(Status::Cancelled) == handleabi::ERR_CANCELLED, "errors: Cancelled is CANCELLED");
    expect(handleabi::error_of(Status::TableFull) == userabi::ERR_LIMIT && handleabi::error_of(Status::NoMemory) == userabi::ERR_LIMIT,
           "errors: a full table and no memory are LIMIT");
    expect(handleabi::error_of(Status::Unsupported) == userabi::ERR_INVAL && handleabi::error_of(Status::Busy) == userabi::ERR_INVAL,
           "errors: a wrong object type and a repeated handle are INVAL");

    i64 codes[] = { userabi::ERR_NOSYS, userabi::ERR_FAULT, userabi::ERR_INVAL, userabi::ERR_LIMIT, handleabi::ERR_AGAIN, handleabi::ERR_CLOSED,
                    handleabi::ERR_DENIED, handleabi::ERR_BADHANDLE, handleabi::ERR_TOOBIG, handleabi::ERR_TIMEDOUT, handleabi::ERR_CANCELLED };
    bool distinct = true;
    for (u32 i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        if (codes[i] != -1 - static_cast<i64>(i)) distinct = false;
        for (u32 j = i + 1; j < sizeof(codes) / sizeof(codes[0]); ++j) if (codes[i] == codes[j]) distinct = false;
    }
    expect(distinct, "errors: the eleven codes are -1 through -11 with no repeats");

    u64 info = handleabi::info_pack(0x103, 3);
    expect(info == 0x30103 && handleabi::info_rights(info) == 0x103 && handleabi::info_kind(info) == 3, "info: rights and kind pack and unpack");
    expect(handleabi::info_pack(lens::RIGHTS_ALL_BITS, 255) > 0, "info: the largest value stays positive as a return value");
    i64 packed = handleabi::recv_pack(256, 4);
    expect(packed > 0 && handleabi::recv_bytes(packed) == 256 && handleabi::recv_handles(packed) == 4, "recv: bytes and handle count pack and unpack");
    expect(handleabi::recv_pack(0, 0) == 0, "recv: an empty message is zero");
    expect(handleabi::recv_pack(0xFFFFFFFFu, 0x7FFFFFFFu) > 0, "recv: even extreme values never look like an error");
    expect(handleabi::wait_want_ok(1) && handleabi::wait_want_ok(7) && !handleabi::wait_want_ok(0) && !handleabi::wait_want_ok(8) && !handleabi::wait_want_ok(0x100),
           "wait: only the three readiness bits are accepted, and at least one");
    expect(lens::RIGHTS_ALL_BITS == 0x3FF, "rights: the ten rights occupy the low ten bits");
    expect(static_cast<u16>(lens::Right::Read) == 1 && static_cast<u16>(lens::Right::Write) == 2 && static_cast<u16>(lens::Right::Delegate) == 0x100,
           "rights: the bit values the ring-3 test programs assume");
    expect(static_cast<u16>(lens::Right::Execute) == 0x200, "rights: execute is bit nine");
}

static void vm_abi_tests() {
    using vmregion::Status;
    expect(userabi::SYS_VMO_CREATE == 18 && userabi::SYS_VM_MAP == 19 && userabi::SYS_VM_UNMAP == 20 && userabi::SYS_VM_PROTECT == 21 &&
           userabi::SYS_VMO_SIZE == 22 && userabi::SYS_COUNT == 33, "vm abi: syscall numbers 18 to 22");
    expect(handleabi::ERR_EXISTS == -12 && handleabi::ERR_NOTMAPPED == -13, "vm abi: new error codes continue the series");
    expect(handleabi::vm_error(Status::Ok) == 0 && handleabi::vm_error(Status::Invalid) == userabi::ERR_INVAL &&
           handleabi::vm_error(Status::Denied) == handleabi::ERR_DENIED && handleabi::vm_error(Status::Exists) == handleabi::ERR_EXISTS &&
           handleabi::vm_error(Status::Limit) == userabi::ERR_LIMIT && handleabi::vm_error(Status::NotMapped) == handleabi::ERR_NOTMAPPED,
           "vm abi: every region status maps to its own code");
    u64 info = handleabi::info_pack(0x3FF, 3, handleabi::SUBTYPE_MEMOBJ);
    expect(handleabi::info_rights(info) == 0x3FF && handleabi::info_kind(info) == 3 && handleabi::info_subtype(info) == 2 && info > 0,
           "vm abi: info carries the stream subtype and stays positive");
    expect(handleabi::info_pack(0x103, 3) == 0x30103, "vm abi: the subtype defaults to none and the old packing is unchanged");

    expect(vmregion::pte_flags(vmregion::PROT_READ) == vmregion::PTE_NO_EXECUTE, "vm abi: read maps NX");
    expect(vmregion::pte_flags(vmregion::PROT_READ | vmregion::PROT_WRITE) == (vmregion::PTE_NO_EXECUTE | vmregion::PTE_WRITABLE), "vm abi: read-write maps NX and writable");
    expect(vmregion::pte_flags(vmregion::PROT_READ | vmregion::PROT_EXEC) == 0, "vm abi: read-execute maps executable and read-only");
    bool wx = false;
    for (u32 prot = 0; prot < 16; ++prot) {
        if (vmregion::check_prot(prot, vmregion::PROT_ALL) != Status::Ok) continue;
        u64 f = vmregion::pte_flags(prot);
        if ((f & vmregion::PTE_WRITABLE) && !(f & vmregion::PTE_NO_EXECUTE)) wx = true;
    }
    expect(!wx, "vm abi: no accepted protection yields a writable and executable page");
}

static u64 xs_state = 0x9E3779B97F4A7C15ull;
static u64 xs() {
    xs_state ^= xs_state << 13;
    xs_state ^= xs_state >> 7;
    xs_state ^= xs_state << 17;
    return xs_state;
}

static bool apx_oracle(const u8* b, u64 n) {
    if (n < 64) return false;
    auto r32 = [&](int o) { return static_cast<u64>(b[o]) | (static_cast<u64>(b[o + 1]) << 8) | (static_cast<u64>(b[o + 2]) << 16) | (static_cast<u64>(b[o + 3]) << 24); };
    if (r32(0) != 0x31585041ull) return false;
    if ((b[4] | (b[5] << 8)) != 1) return false;
    if ((b[6] | (b[7] << 8)) != 64) return false;
    for (int i = 44; i < 64; ++i) if (b[i]) return false;
    u64 flags = r32(8), coff = r32(12), cbytes = r32(16), entry = r32(20), dpages = r32(24), doff = r32(28), dbytes = r32(32), fbytes = r32(36), sum = r32(40);
    if (flags) return false;
    if (cbytes == 0 || cbytes > 64ull * 4096) return false;
    if (dpages > 64) return false;
    if (dbytes > dpages * 4096) return false;
    if (entry >= cbytes) return false;
    if (coff % 16 || doff % 16) return false;
    if (coff < 64) return false;
    if (n > 64 + 2ull * 64 * 4096) return false;
    if (fbytes != n) return false;
    if (coff + cbytes > n) return false;
    u64 end = coff + cbytes;
    if (dbytes == 0) {
        if (doff != 0) return false;
    } else {
        if (doff < 64 || doff + dbytes > n) return false;
        bool disjoint = doff + dbytes <= coff || doff >= coff + cbytes;
        if (!disjoint) return false;
        if (doff + dbytes > end) end = doff + dbytes;
    }
    if (end != n) return false;
    u32 c = 0xFFFFFFFFu;
    for (u64 i = 64; i < n; ++i) {
        c ^= b[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return static_cast<u64>(~c) == sum;
}

static usize apx_build(u8* out, usize cap, u32 code_bytes, u32 entry, u32 dpages, u32 dbytes, u8 seed) {
    static u8 code[apx::MAX_CODE_BYTES + 16];
    static u8 data[apx::MAX_DATA_BYTES + 16];
    for (u32 i = 0; i < code_bytes; ++i) code[i] = static_cast<u8>(seed + i * 7);
    for (u32 i = 0; i < dbytes; ++i) data[i] = static_cast<u8>(seed ^ (i * 13));
    usize w = 0;
    if (!apx::pack(code, code_bytes, entry, dpages, data, dbytes, out, cap, &w)) return 0;
    return w;
}

static void apx_tests() {
    static u8 buf[apx::MAX_FILE_BYTES + 64];

    struct Tiny {
        u8 bytes[128];
        usize n;
    };
    constexpr Tiny tiny = [] {
        constexpr u8 tiny_code[4] = { 0x90, 0x90, 0xC3, 0xCC };
        Tiny t{};
        apx::pack(tiny_code, 4, 2, 1, nullptr, 0, t.bytes, 128, &t.n);
        return t;
    }();
    static_assert(tiny.n == 68, "a header plus four code bytes");
    static_assert(apx::parse(tiny.bytes, tiny.n).status == apx::Status::Ok, "a packed image parses at compile time");
    expect(apx::crc32(reinterpret_cast<const u8*>("123456789"), 9) == 0xCBF43926u, "crc32 matches the standard check value");

    struct Shape { u32 code, entry, dpages, dbytes; };
    const Shape shapes[] = {
        { 1, 0, 0, 0 }, { 4096, 4095, 0, 0 }, { 100, 50, 1, 0 }, { 100, 99, 1, 1 }, { 4097, 4096, 2, 4096 },
        { 17, 3, 8, 8 * 4096 }, { 64 * 4096, 64 * 4096 - 1, 64, 64 * 4096 }, { 64 * 4096, 0, 0, 0 }, { 33, 32, 64, 4095 },
    };
    bool all_round = true;
    for (const Shape& sh : shapes) {
        usize n = apx_build(buf, sizeof(buf), sh.code, sh.entry, sh.dpages, sh.dbytes, static_cast<u8>(sh.code));
        apx::Parsed p = apx::parse(buf, n);
        bool ok = n != 0 && p.status == apx::Status::Ok && p.header.code_bytes == sh.code && p.header.entry_offset == sh.entry &&
                  p.header.data_pages == sh.dpages && p.header.data_bytes == sh.dbytes && p.header.file_bytes == n &&
                  p.header.code_offset == 64 && apx_oracle(buf, n);
        if (ok && sh.dbytes) ok = p.header.data_offset % 16 == 0 && p.header.data_offset >= 64 + sh.code && p.header.data_offset < 64 + sh.code + 16;
        if (!ok) all_round = false;
    }
    expect(all_round, "apx: packed images of every shape parse and match the oracle");

    expect(apx_build(buf, sizeof(buf), 64 * 4096 + 1, 0, 0, 0, 1) != 0 ? apx::parse(buf, apx::pack_size(64 * 4096 + 1, 0)).status == apx::Status::CodeTooLarge : true,
           "apx: code above the limit is refused");

    usize n = apx_build(buf, sizeof(buf), 300, 17, 2, 120, 5);
    static u8 good[apx::MAX_FILE_BYTES + 64];
    for (usize i = 0; i < n; ++i) good[i] = buf[i];

    bool trunc_ok = true;
    for (usize t = 0; t < n; ++t) {
        apx::Parsed p = apx::parse(good, t);
        if (p.status == apx::Status::Ok || apx_oracle(good, t)) trunc_ok = false;
    }
    expect(trunc_ok, "apx: every truncation of a valid image is refused");

    bool grow_ok = true;
    for (usize extra = 1; extra < 40; ++extra) {
        for (usize i = 0; i < n; ++i) buf[i] = good[i];
        for (usize i = 0; i < extra; ++i) buf[n + i] = static_cast<u8>(i);
        if (apx::parse(buf, n + extra).status == apx::Status::Ok) grow_ok = false;
    }
    expect(grow_ok, "apx: trailing bytes are refused");

    u64 mismatches = 0, accepted = 0, rejected = 0;
    for (usize pos = 0; pos < n; ++pos) {
        for (u32 v = 0; v < 256; ++v) {
            if (v == good[pos]) continue;
            for (usize i = 0; i < n; ++i) buf[i] = good[i];
            buf[pos] = static_cast<u8>(v);
            bool a = apx::parse(buf, n).status == apx::Status::Ok;
            bool o = apx_oracle(buf, n);
            if (a != o) ++mismatches;
            if (a) ++accepted; else ++rejected;
            if (pos >= 64 && a) ++mismatches;
        }
    }
    expect(mismatches == 0, "apx: every single-byte mutation agrees with the independent oracle");
    expect(rejected > accepted, "apx: most single-byte mutations are refused");
    printf("    (%llu accepted, %llu refused)\n", static_cast<unsigned long long>(accepted), static_cast<unsigned long long>(rejected));

    u64 bit_miss = 0;
    for (usize pos = 0; pos < n; ++pos) {
        for (int bit = 0; bit < 8; ++bit) {
            for (usize i = 0; i < n; ++i) buf[i] = good[i];
            buf[pos] ^= static_cast<u8>(1u << bit);
            if ((apx::parse(buf, n).status == apx::Status::Ok) != apx_oracle(buf, n)) ++bit_miss;
        }
    }
    expect(bit_miss == 0, "apx: every single-bit flip agrees with the oracle");

    u64 field_miss = 0, field_ok = 0;
    for (int round = 0; round < 400000; ++round) {
        for (usize i = 0; i < n; ++i) buf[i] = good[i];
        int fields = 1 + static_cast<int>(xs() % 3);
        for (int f = 0; f < fields; ++f) {
            int off = static_cast<int>((xs() % 11) * 4);
            u32 v;
            switch (xs() % 7) {
                case 0: v = 0; break;
                case 1: v = 0xFFFFFFFFu; break;
                case 2: v = 0xFFFFFFF0u; break;
                case 3: v = static_cast<u32>(xs()); break;
                case 4: v = static_cast<u32>(xs() % 0x20000); break;
                case 5: v = static_cast<u32>(n + (xs() % 5)) - 2; break;
                default: v = static_cast<u32>(xs() % 600); break;
            }
            if (off == 0 && (xs() & 3)) continue;
            if (off == 4 && (xs() & 3)) continue;
            apx::wr32(buf + off, v);
        }
        if (xs() & 1) {
            u32 crc = apx::crc32(buf + 64, n - 64);
            apx::wr32(buf + 40, crc);
        }
        bool a = apx::parse(buf, n).status == apx::Status::Ok;
        bool o = apx_oracle(buf, n);
        if (a != o) ++field_miss;
        if (a) ++field_ok;
    }
    expect(field_miss == 0, "apx: 400k random header-field rewrites agree with the oracle");
    expect(field_ok > 0, "apx: the field fuzz reached accepted images");

    u64 rand_miss = 0;
    for (int round = 0; round < 20000; ++round) {
        usize len = static_cast<usize>(xs() % 300);
        for (usize i = 0; i < len; ++i) buf[i] = static_cast<u8>(xs());
        if (apx::parse(buf, len).status == apx::Status::Ok || apx_oracle(buf, len)) ++rand_miss;
    }
    expect(rand_miss == 0, "apx: random garbage is never accepted");

    u64 big_miss = 0;
    for (int round = 0; round < 250; ++round) {
        u32 cb = 1 + static_cast<u32>(xs() % (64 * 4096));
        u32 dp = static_cast<u32>(xs() % 65);
        u32 db = dp ? static_cast<u32>(xs() % (dp * 4096ull + 1)) : 0;
        u32 en = static_cast<u32>(xs() % cb);
        usize w = apx_build(buf, sizeof(buf), cb, en, dp, db, static_cast<u8>(round));
        if (!w) { ++big_miss; continue; }
        if (apx::parse(buf, w).status != apx::Status::Ok || !apx_oracle(buf, w)) ++big_miss;
        if (apx::parse(buf, w - 1).status == apx::Status::Ok) ++big_miss;
        usize flip = static_cast<usize>(xs() % w);
        buf[flip] ^= static_cast<u8>(1u << (xs() % 8));
        if ((apx::parse(buf, w).status == apx::Status::Ok) != apx_oracle(buf, w)) ++big_miss;
    }
    expect(big_miss == 0, "apx: 250 random shapes round-trip, refuse truncation, and agree with the oracle after a flip");

    for (usize i = 0; i < n; ++i) buf[i] = good[i];
    apx::Parsed head = apx::parse_header(buf, 64, n);
    expect(head.status == apx::Status::Ok, "apx: the header alone validates against the full file size");
    expect(apx::parse_header(buf, 63, n).status == apx::Status::Truncated, "apx: a short header read is Truncated");
    expect(apx::parse_header(buf, 64, n + 1).status == apx::Status::SizeMismatch, "apx: a wrong file size is refused");
    expect(apx::parse_header(buf, 64, 0xFFFFFFFFFFull).status == apx::Status::SizeMismatch, "apx: an enormous file size is refused");
    const char* names[] = { apx::status_name(apx::Status::Ok), apx::status_name(apx::Status::BadChecksum), apx::status_name(apx::Status::Overlap) };
    expect(names[0][0] == 'o' && names[1][0] == 'c' && names[2][0] == 's', "apx: status names are populated");
}

static usize apx_forge(u8* out, usize cap, u32 code_off, u32 code_bytes, u32 entry, u32 dpages, u32 data_off, u32 data_bytes, usize total) {
    if (total > cap) return 0;
    for (usize i = 0; i < total; ++i) out[i] = static_cast<u8>(i * 31 + 7);
    apx::Header h{};
    h.code_offset = code_off;
    h.code_bytes = code_bytes;
    h.entry_offset = entry;
    h.data_pages = dpages;
    h.data_offset = data_off;
    h.data_bytes = data_bytes;
    h.file_bytes = static_cast<u32>(total);
    h.checksum = apx::crc32(out + 64, total - 64);
    apx::encode(h, out);
    return total;
}

static void apx_layout_tests() {
    static u8 big[0x100200];
    auto st = [&](u32 co, u32 cb, u32 en, u32 dp, u32 doff, u32 db, usize total) {
        usize n = apx_forge(big, sizeof(big), co, cb, en, dp, doff, db, total);
        return apx::parse(big, n).status;
    };
    expect(st(64, 100, 0, 1, 176, 16, 192) == apx::Status::Ok, "apx layout: code then aligned data is accepted");
    expect(st(64, 100, 0, 1, 165, 16, 181) == apx::Status::Misaligned, "apx layout: an unaligned data section is refused");
    expect(st(65, 100, 0, 1, 0, 0, 165) == apx::Status::Misaligned, "apx layout: an unaligned code section is refused");
    expect(st(64, 100, 0, 1, 16, 16, 164) == apx::Status::BadDataRange, "apx layout: data inside the header is refused");
    expect(st(128, 100, 0, 0, 0, 0, 228) == apx::Status::Ok, "apx layout: padding between the header and the code is accepted");
    expect(st(0x100000, 16, 0, 0, 0, 0, 0x100010) == apx::Status::SizeMismatch, "apx layout: a padded file above the size cap is refused");
    expect(st(64 + 4096, 16, 0, 1, 64, 16, 64 + 4096 + 16) == apx::Status::Ok, "apx layout: data placed before the code is accepted");
    expect(st(64, 100, 0, 1, 80, 16, 164) == apx::Status::Overlap, "apx layout: data inside the code is refused");
    expect(st(64, 100, 100, 1, 176, 16, 192) == apx::Status::BadEntry, "apx layout: an entry one past the code is refused");
    expect(st(64, 100, 99, 1, 176, 16, 192) == apx::Status::Ok, "apx layout: the last code byte is a valid entry");
    expect(st(64, 100, 0, 0, 176, 16, 192) == apx::Status::DataTooLarge, "apx layout: data without pages is refused");
    expect(st(64, 100, 0, 1, 0, 0, 164) == apx::Status::Ok, "apx layout: data pages without initial data are accepted");
    expect(st(64, 100, 0, 1, 64, 0, 164) == apx::Status::BadDataRange, "apx layout: a data offset without data is refused");
}

static void file_abi_tests() {
    using lens::Status;
    static_assert(userabi::SYS_FILE_OPEN == 23 && userabi::SYS_PROCESS_KILL == 32, "syscall numbers 23 to 32");
    expect(userabi::SYS_FILE_OPEN == 23 && userabi::SYS_FILE_READ == 24 && userabi::SYS_FILE_WRITE == 25 && userabi::SYS_FILE_STAT == 26 &&
               userabi::SYS_FILE_CREATE == 27 && userabi::SYS_DIR_ENTRY == 28 && userabi::SYS_PIPE_CREATE == 29 &&
               userabi::SYS_PROCESS_CREATE == 30 && userabi::SYS_PROCESS_STATUS == 31 && userabi::SYS_PROCESS_KILL == 32,
           "file abi: syscall numbers 23 to 32");
    expect(fileabi::path_len_ok(1) && fileabi::path_len_ok(255) && !fileabi::path_len_ok(0) && !fileabi::path_len_ok(256) && !fileabi::path_len_ok(~0ull),
           "file abi: path lengths 1 to 255 only");
    expect(fileabi::path_text_ok("a/b", 3) && !fileabi::path_text_ok("a\0b", 3) && !fileabi::path_text_ok("a", 0), "file abi: embedded NULs are refused");
    expect(fileabi::rights_ok(0x3FF) && fileabi::rights_ok(0) && !fileabi::rights_ok(0x400) && !fileabi::rights_ok(0x10000) && !fileabi::rights_ok(1ull << 40),
           "file abi: only the ten defined rights are accepted");
    expect(fileabi::clamp_io(0) == 0 && fileabi::clamp_io(1) == 1 && fileabi::clamp_io(1024) == 1024 && fileabi::clamp_io(1025) == 1024 && fileabi::clamp_io(~0ull) == 1024,
           "file abi: a transfer is clamped to 1024 bytes");
    expect(fileabi::error_of(Status::NotFound) == fileabi::ERR_NOTFOUND && fileabi::error_of(Status::Exists) == handleabi::ERR_EXISTS &&
               fileabi::error_of(Status::NotADirectory) == fileabi::ERR_WRONGTYPE && fileabi::error_of(Status::IsADirectory) == fileabi::ERR_WRONGTYPE &&
               fileabi::error_of(Status::NotAFile) == fileabi::ERR_WRONGTYPE,
           "file abi: not-found, exists and wrong-type have their own codes");
    expect(fileabi::error_of(Status::Denied) == handleabi::ERR_DENIED && fileabi::error_of(Status::NotReady) == handleabi::ERR_AGAIN &&
               fileabi::error_of(Status::Closed) == handleabi::ERR_CLOSED && fileabi::error_of(Status::StaleLens) == handleabi::ERR_BADHANDLE &&
               fileabi::error_of(Status::NoSpace) == userabi::ERR_LIMIT,
           "file abi: other statuses keep the handle ABI codes");
    bool distinct = true;
    const i64 codes[] = { userabi::ERR_NOSYS, userabi::ERR_FAULT, userabi::ERR_INVAL, userabi::ERR_LIMIT, handleabi::ERR_AGAIN, handleabi::ERR_CLOSED,
                          handleabi::ERR_DENIED, handleabi::ERR_BADHANDLE, handleabi::ERR_TOOBIG, handleabi::ERR_TIMEDOUT, handleabi::ERR_CANCELLED,
                          handleabi::ERR_EXISTS, handleabi::ERR_NOTMAPPED, handleabi::ERR_BADIMAGE, fileabi::ERR_NOTFOUND, fileabi::ERR_WRONGTYPE };
    for (usize i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i)
        for (usize j = i + 1; j < sizeof(codes) / sizeof(codes[0]); ++j)
            if (codes[i] == codes[j]) distinct = false;
    expect(distinct, "file abi: every error code is distinct");
    expect(fileabi::stat_flags(false, false) == 0 && fileabi::stat_flags(true, false) == fileabi::STAT_READ_ONLY &&
               fileabi::stat_flags(false, true) == fileabi::STAT_DYNAMIC && fileabi::stat_flags(true, true) == 3,
           "file abi: stat flags");
    expect(handleabi::info_subtype(handleabi::info_pack(5, 3, static_cast<u8>(handleabi::SUBTYPE_PROCESS))) == 3, "file abi: the process subtype round-trips through handle_info");
    expect(fileabi::PROCESS_RIGHTS.has(lens::Right::Read) && fileabi::PROCESS_RIGHTS.has(lens::Right::Write) && fileabi::PROCESS_RIGHTS.has(lens::Right::Delegate) &&
               !fileabi::PROCESS_RIGHTS.has(lens::Right::Execute),
           "file abi: a process handle carries read, write and delegate");
    expect(fileabi::LAUNCH_RIGHTS.has_all(lens::Right::Read | lens::Right::Execute) && !fileabi::LAUNCH_RIGHTS.has(lens::Right::Write),
           "file abi: launching needs read and execute");
    expect(sizeof(fileabi::Stat) == 32 && sizeof(fileabi::DirEntry) == 280 && fileabi::MAX_PIPE_BYTES == 65536, "file abi: record sizes");
    expect(apx::MAX_FILE_BYTES == 64 + 64ull * 4096 * 2, "file abi: the largest image is a header plus the code and data limits");
    expect(userabi::CODE_BASE + static_cast<u64>(userabi::MAX_CODE_PAGES) * 4096 <= userabi::DATA_BASE &&
               userabi::DATA_BASE + static_cast<u64>(userabi::MAX_DATA_PAGES) * 4096 < userabi::STACK_TOP - userabi::STACK_PAGES * 4096ull,
           "file abi: the raised code and data limits still fit their regions");
}

int main() {
    entry_fence_tests();
    range_tests();
    copy_tests();
    abi_tests();
    gs_base_tests();
    gs_model_test();
    fs_gs_pair_model_test();
    abort_policy_tests();
    procgroup_tests();
    procgroup_model_test();
    procgroup_stress_test();
    handle_abi_tests();
    vmregion_tests();
    vm_abi_tests();
    apx_tests();
    apx_layout_tests();
    file_abi_tests();
    printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
