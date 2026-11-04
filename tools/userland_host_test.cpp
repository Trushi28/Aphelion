#include <cosmos/types.hpp>
#include <cosmos/userabi.hpp>
#include <cosmos/usercopy.hpp>
#include <cosmos/usergs.hpp>
#include <cosmos/ringabort.hpp>
#include <cosmos/entrycheck.hpp>
#include <cosmos/procgroup.hpp>
#include <cosmos/handleabi.hpp>
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
    static_assert(userabi::SYS_COUNT == 18, "the native table has eighteen entries in this series");
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
    expect(lens::RIGHTS_ALL_BITS == 0x1FF, "rights: the nine rights occupy the low nine bits");
    expect(static_cast<u16>(lens::Right::Read) == 1 && static_cast<u16>(lens::Right::Write) == 2 && static_cast<u16>(lens::Right::Delegate) == 0x100,
           "rights: the bit values the ring-3 test programs assume");
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
    printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
