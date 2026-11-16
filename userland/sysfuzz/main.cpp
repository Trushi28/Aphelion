#include <begin.hpp>
#include <cosmos/handleabi.hpp>

extern "C" void fuzz_syscall(const u64* in, u64* out);

asm(R"(
.pushsection .text
.globl fuzz_syscall
.type fuzz_syscall,@function
fuzz_syscall:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushq %rsi
    movq %rdi, %r11
    movabsq $0x1111111111111111, %rbx
    movabsq $0x2222222222222222, %rbp
    movabsq $0x3333333333333333, %r12
    movabsq $0x4444444444444444, %r13
    movabsq $0x5555555555555555, %r14
    movabsq $0x6666666666666666, %r15
    movq 0(%r11), %rax
    movq 8(%r11), %rdi
    movq 16(%r11), %rsi
    movq 24(%r11), %rdx
    movq 32(%r11), %r10
    movq 40(%r11), %r8
    movq 48(%r11), %r9
    syscall
    movq (%rsp), %rcx
    movq %rax, 0(%rcx)
    movq %rbx, 8(%rcx)
    movq %rbp, 16(%rcx)
    movq %r12, 24(%rcx)
    movq %r13, 32(%rcx)
    movq %r14, 40(%rcx)
    movq %r15, 48(%rcx)
    movq %rdi, 56(%rcx)
    movq %rsi, 64(%rcx)
    movq %rdx, 72(%rcx)
    movq %r10, 80(%rcx)
    movq %r8, 88(%rcx)
    movq %r9, 96(%rcx)
    addq $8, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret
.size fuzz_syscall, .-fuzz_syscall
.popsection
)");

namespace {

using namespace userabi;

constexpr u64 WINDOW = 0x100000000000ull;
constexpr u64 RACE_X = WINDOW + 0x1000000;
constexpr u64 RACE_Y = WINDOW + 0x1100000;
constexpr u64 EDGE = WINDOW + 0x1200000;
constexpr u64 SWEEP_PAGES = 256;
constexpr u64 OUT = 0xDEADBEEFCAFEF00Dull;
constexpr i64 ERR_LOWEST = -19;
constexpr u32 SWEEP_CALLS = 6000;
constexpr u32 RACE_CALLS = 1500;
constexpr u32 HANDLE_RACE_CALLS = 2500;
constexpr u32 MAX_REPORTS = 24;

struct Line {
    char text[221];
    u64 used = 0;

    void add(const char* s) {
        while (*s && used < sizeof(text) - 1) text[used++] = *s++;
    }

    void add(u64 v) {
        char t[20];
        u32 n = 0;
        do {
            t[n++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v);
        while (n && used < sizeof(text) - 1) text[used++] = t[--n];
    }

    void addi(i64 v) {
        if (v < 0) {
            add("-");
            add(static_cast<u64>(-v));
        } else {
            add(static_cast<u64>(v));
        }
    }

    void hex(u64 v) {
        add("0x");
        bool started = false;
        for (int shift = 60; shift >= 0; shift -= 4) {
            u32 d = static_cast<u32>((v >> shift) & 15);
            if (d == 0 && !started && shift != 0) continue;
            started = true;
            if (used < sizeof(text) - 1) text[used++] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10);
        }
    }

    const char* c_str() {
        text[used] = 0;
        return text;
    }

    void flush() {
        sys::debug_write(text, used);
        used = 0;
    }
};

u64 g_seed = 0x9E3779B97F4A7C15ull;
u64 g_calls;
u64 g_register_checks;
u64 g_anomalies;
u64 g_failed_checks;
u8 g_succeeded[SYS_COUNT];

alignas(4096) u8 g_scratch[8192];
alignas(4096) u8 g_stacks[2][8192];

u64 g_pool[16];
u32 g_pool_count;
u64 g_stale[16];
u32 g_stale_count;
u64 g_harvest[192];
u32 g_harvest_count;
u64 g_capacity_pairs;

volatile u32 g_stop;

u64 rnd() {
    g_seed ^= g_seed >> 12;
    g_seed ^= g_seed << 25;
    g_seed ^= g_seed >> 27;
    return g_seed * 0x2545F4914F6CDD1Dull;
}

u64 below(u64 n) { return rnd() % n; }

struct Raw {
    i64 value;
    bool registers_kept;
};

Raw raw_call(u64 nr, const u64* a) {
    u64 in[7] = { nr, a[0], a[1], a[2], a[3], a[4], a[5] };
    u64 out[13];
    fuzz_syscall(in, out);
    bool kept = out[1] == 0x1111111111111111ull && out[2] == 0x2222222222222222ull && out[3] == 0x3333333333333333ull &&
                out[4] == 0x4444444444444444ull && out[5] == 0x5555555555555555ull && out[6] == 0x6666666666666666ull &&
                out[7] == a[0] && out[8] == a[1] && out[9] == a[2] && out[10] == a[3] && out[11] == a[4] && out[12] == a[5];
    return { static_cast<i64>(out[0]), kept };
}

void report(const char* why, u64 nr, const u64* a, i64 value) {
    ++g_anomalies;
    if (g_anomalies > MAX_REPORTS) return;
    Line l;
    l.add("[fuzz] ANOMALY ");
    l.add(why);
    l.add(": syscall ");
    l.add(nr);
    l.add(" result ");
    l.addi(value);
    for (u32 i = 0; i < 6; ++i) {
        l.add(i ? " " : " args ");
        l.hex(a[i]);
    }
    l.flush();
}

void check(bool good, const char* what) {
    if (good) return;
    ++g_failed_checks;
    Line l;
    l.add("[fuzz] FAILED: ");
    l.add(what);
    l.flush();
}

bool plausible(u64 nr, i64 value) {
    if (nr >= SYS_COUNT) return value == ERR_NOSYS;
    if (value == ERR_NOSYS) return false;
    return value >= ERR_LOWEST;
}

bool in_pool(u64 h) {
    for (u32 i = 0; i < g_pool_count; ++i)
        if (g_pool[i] == h) return true;
    return false;
}

void harvest(const u64* a) {
    for (u32 k = 0; k < 6; ++k) {
        u64 p = a[k];
        if (p < reinterpret_cast<u64>(g_scratch) || p >= reinterpret_cast<u64>(g_scratch) + 4096) continue;
        for (u32 w = 0; w < 6; ++w) {
            u64 value = reinterpret_cast<const u64*>(p)[w];
            if (value == 0 || in_pool(value) || g_harvest_count >= sizeof(g_harvest) / sizeof(g_harvest[0])) continue;
            bool known = false;
            for (u32 i = 0; i < g_harvest_count; ++i) known = known || g_harvest[i] == value;
            if (known) continue;
            if (sys::call(SYS_HANDLE_INFO, value) >= 0) g_harvest[g_harvest_count++] = value;
        }
    }
}

i64 do_call(u64 nr, const u64* a) {
    Raw r = raw_call(nr, a);
    ++g_calls;
    ++g_register_checks;
    if (!r.registers_kept) report("registers not preserved", nr, a, r.value);
    if (!plausible(nr, r.value)) report("result outside the documented range", nr, a, r.value);
    if (r.value >= 0 && nr < SYS_COUNT) g_succeeded[nr] = 1;
    harvest(a);
    return r.value;
}

i64 call6(u64 nr, u64 a, u64 b = 0, u64 c = 0, u64 d = 0, u64 e = 0, u64 f = 0) {
    u64 args[6] = { a, b, c, d, e, f };
    return do_call(nr, args);
}

u64 scratch_at(u64 offset) { return reinterpret_cast<u64>(g_scratch) + offset; }

void close_harvested() {
    for (u32 i = 0; i < g_harvest_count; ++i) sys::handle_close(g_harvest[i]);
    g_harvest_count = 0;
}

void close_pool() {
    for (u32 i = 0; i < g_pool_count; ++i) sys::handle_close(g_pool[i]);
    g_pool_count = 0;
}

void remember_stale(u64 h) { g_stale[g_stale_count++ % (sizeof(g_stale) / sizeof(g_stale[0]))] = h; }

void refresh_pool() {
    for (u32 i = 0; i < g_pool_count; ++i) {
        sys::handle_close(g_pool[i]);
        remember_stale(g_pool[i]);
    }
    g_pool_count = 0;
    u64 pair[2] = {};
    for (u32 i = 0; i < 2; ++i) {
        if (call6(SYS_CHANNEL_CREATE, reinterpret_cast<u64>(pair)) == 0) {
            g_pool[g_pool_count++] = pair[0];
            g_pool[g_pool_count++] = pair[1];
        }
    }
    u64 vmo = 0;
    if (sys::vmo_create(2, &vmo) == 0) g_pool[g_pool_count++] = vmo;
    u64 pipe[2] = {};
    if (sys::pipe_create(16, pipe) == 0) {
        g_pool[g_pool_count++] = pipe[0];
        g_pool[g_pool_count++] = pipe[1];
    }
}

u64 pool_pick() { return g_pool_count ? g_pool[below(g_pool_count)] : 0x7777777700000001ull; }

u64 stale_pick() { return g_stale_count ? g_stale[below(g_stale_count < 16 ? g_stale_count : 16)] : 0x7777777700000002ull; }

u64 interesting() {
    switch (below(14)) {
    case 0: return 0;
    case 1: return 1;
    case 2: return below(9);
    case 3: return ~0ull;
    case 4: return USER_LIMIT;
    case 5: return USER_LIMIT - 1 - below(4096);
    case 6: return 0xFFFF800000000000ull + below(1ull << 20);
    case 7: return 0x1000 + below(0x3FE) * 0x1000;
    case 8: return pool_pick();
    case 9: return stale_pick();
    case 10: return scratch_at(below(4096));
    case 11: return WINDOW + below(64) * 0x1000 + below(2) * 8;
    case 12: return below(1ull << 22);
    default: return rnd();
    }
}

u64 pick(const u64* values, u32 count) { return values[below(count)]; }

void shape(u64 nr, u64* a) {
    static const u64 pages[] = { 0, 1, 2, 3, 8, 4097, ~0ull };
    static const u64 prots[] = { 1, 3, 5, 7, 0, 2, 4, 0x100000001ull };
    switch (nr) {
    case SYS_DEBUG_WRITE:
        if (a[1] != 0 && a[1] <= DEBUG_WRITE_MAX) a[0] = 8;
        break;
    case SYS_FUTEX_WAIT:
        a[2] = 1 + below(2);
        break;
    case SYS_HANDLE_WAIT:
        a[2] = below(3);
        if (below(2)) a[1] = 1 + below(3);
        break;
    case SYS_CHANNEL_RECV:
        a[5] &= ~1ull;
        if (below(2)) {
            a[0] = pool_pick();
            a[1] = scratch_at(0);
            a[2] = below(300);
            a[3] = scratch_at(512);
            a[4] = below(7);
        }
        break;
    case SYS_CHANNEL_SEND:
        if (below(2)) {
            reinterpret_cast<u64*>(g_scratch + 768)[0] = pool_pick();
            reinterpret_cast<u64*>(g_scratch + 768)[1] = pool_pick();
            a[0] = pool_pick();
            a[1] = scratch_at(0);
            a[2] = below(8);
            a[3] = scratch_at(768);
            a[4] = below(4);
        }
        break;
    case SYS_VMO_CREATE:
        a[0] = pick(pages, 7);
        if (below(4)) a[1] = 0;
        if (below(2)) a[2] = scratch_at(8 * below(64));
        break;
    case SYS_VM_MAP:
        if (below(10) < 6) {
            a[0] = pool_pick();
            a[1] = below(2) ? 0 : WINDOW + 0x1000 * below(64);
            a[2] = below(3);
            a[3] = 1 + below(4);
            a[4] = pick(prots, 8);
        }
        break;
    case SYS_VM_UNMAP:
    case SYS_VM_PROTECT:
        if (below(2)) {
            a[0] = WINDOW + 0x1000 * below(64);
            a[1] = 1 + below(4);
            a[2] = pick(prots, 8);
        }
        break;
    case SYS_CHANNEL_CREATE:
    case SYS_PIPE_CREATE:
        if (below(2)) a[nr == SYS_PIPE_CREATE ? 1 : 0] = scratch_at(16 * below(32));
        if (nr == SYS_PIPE_CREATE && below(2)) a[0] = 1 + below(64);
        break;
    default:
        break;
    }
}

bool sweepable(u64 nr) { return nr != SYS_EXIT && nr != SYS_THREAD_EXIT && nr != SYS_THREAD_CREATE; }

void sweep() {
    refresh_pool();
    for (u32 i = 0; i < SWEEP_CALLS; ++i) {
        if (i % 100 == 0) {
            close_harvested();
            refresh_pool();
        }
        u64 nr = below(40) == 0 ? SYS_COUNT + below(1000) : below(SYS_COUNT);
        if (nr < SYS_COUNT && !sweepable(nr)) continue;
        u64 a[6];
        for (u32 k = 0; k < 6; ++k) a[k] = interesting();
        shape(nr, a);
        do_call(nr, a);
    }
    close_harvested();
    close_pool();
    for (u64 page = 0; page < SWEEP_PAGES; ++page) sys::vm_unmap(WINDOW + page * 0x1000, 1);
}

u64 capacity_pairs() {
    u64 pairs[80][2];
    u64 n = 0;
    while (n < 80) {
        i64 r = sys::call(SYS_CHANNEL_CREATE, reinterpret_cast<u64>(pairs[n]));
        if (r != 0) {
            check(r == ERR_LIMIT, "a full handle table must report the limit error");
            break;
        }
        ++n;
    }
    for (u64 i = 0; i < n; ++i) {
        sys::handle_close(pairs[i][0]);
        sys::handle_close(pairs[i][1]);
    }
    return n;
}

void edges() {
    static const u64 beyond[] = { SYS_COUNT, SYS_COUNT + 1, 1000, 0xFFFFFFFFull, 1ull << 32, 1ull << 63, ~0ull };
    for (u64 nr : beyond) check(call6(nr, 1, 2, 3, 4, 5, 6) == ERR_NOSYS, "a call number past the table must report no such call");
    check(call6(SYS_THREAD_CREATE, 0, STACK_TOP) == ERR_INVAL, "thread_create with a null entry must be refused");
    check(call6(SYS_THREAD_CREATE, CODE_BASE, 0) == ERR_INVAL, "thread_create with a null stack must be refused");
    check(call6(SYS_THREAD_CREATE, USER_LIMIT, STACK_TOP) == ERR_INVAL, "thread_create with a kernel-half entry must be refused");
    check(call6(SYS_THREAD_CREATE, CODE_BASE, USER_LIMIT) == ERR_INVAL, "thread_create with a stack at the user limit must be refused");
    check(call6(SYS_THREAD_CREATE, ~0ull, ~0ull) == ERR_INVAL, "thread_create with all-ones arguments must be refused");
    check(call6(SYS_THREAD_JOIN, 0) == ERR_INVAL, "thread_join of id zero must be refused");
    check(call6(SYS_THREAD_JOIN, 1ull << 32) == ERR_INVAL, "thread_join of an id above 32 bits must be refused");
    check(call6(SYS_THREAD_JOIN, static_cast<u64>(sys::gettid())) == ERR_INVAL, "a thread must not be able to join itself");
    check(call6(SYS_SET_GS_BASE, USER_LIMIT) == ERR_INVAL, "a GS base in the canonical hole must be refused");
    check(call6(SYS_SET_FS_BASE, ~0ull) == ERR_INVAL, "an FS base in the kernel half must be refused");

    static volatile u32 word = 5;
    u64 address = reinterpret_cast<u64>(&word);
    check(call6(SYS_FUTEX_WAIT, address, 6, 5) == handleabi::ERR_AGAIN, "futex_wait on a word that differs must return at once");
    check(call6(SYS_FUTEX_WAIT, address + 1, 5, 5) == ERR_INVAL, "futex_wait on an unaligned address must be refused");
    check(call6(SYS_FUTEX_WAIT, 0, 0, 5) == ERR_INVAL, "futex_wait on null must be refused");
    check(call6(SYS_FUTEX_WAIT, USER_LIMIT - 4, 0, 5) == ERR_INVAL || call6(SYS_FUTEX_WAIT, USER_LIMIT - 4, 0, 5) == ERR_FAULT,
          "futex_wait at the very top of user space must not wait");
    check(call6(SYS_FUTEX_WAIT, address, 5, 2) == handleabi::ERR_TIMEDOUT, "futex_wait with nobody waking must time out");
    check(call6(SYS_FUTEX_WAKE, address, 0) == 0, "futex_wake of zero waiters must return zero");
}

bool page_is(const u8* p, u64 n, u8 value) {
    for (u64 i = 0; i < n; ++i)
        if (p[i] != value) return false;
    return true;
}

void close_words(const u64* words, u64 count) {
    for (u64 i = 0; i < count; ++i)
        if (words[i] != 0 && sys::call(SYS_HANDLE_INFO, words[i]) >= 0) sys::handle_close(words[i]);
}

void straddle(const char* name, u64 nr, u64 size, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    u8* page = reinterpret_cast<u8*>(EDGE);
    for (u64 i = 0; i < 4096; ++i) page[i] = 0xAB;
    u64 split = size / 2;
    u64 bad = EDGE + 4096 - split;
    u64 args[6] = { a0, a1, a2, a3, a4, a5 };
    u64 good[6] = { a0, a1, a2, a3, a4, a5 };
    for (u32 i = 0; i < 6; ++i) {
        if (args[i] == OUT) args[i] = bad;
        if (good[i] == OUT) good[i] = EDGE;
    }
    i64 r = do_call(nr, args);
    Line l;
    l.add("an output buffer that runs into an unmapped page must fail with nothing written: ");
    l.add(name);
    check(r == ERR_FAULT && page_is(page + 4096 - split, split, 0xAB), l.c_str());
    for (u64 i = 0; i < 4096; ++i) page[i] = 0xAB;
    do_call(nr, good);
    Line c;
    c.add("the same call on a fully mapped buffer must write to it: ");
    c.add(name);
    check(!page_is(page, size, 0xAB), c.c_str());
    close_words(reinterpret_cast<const u64*>(page), size / 8 < 6 ? size / 8 : 6);
}

void atomic_copy() {
    u64 vmo = 0;
    check(sys::vmo_create(1, &vmo) == 0, "a memory object can be created for the edge buffer");
    check(sys::vm_map(vmo, EDGE, 0, 1, 3) == static_cast<i64>(EDGE), "the edge buffer maps at its fixed address");
    check(sys::vm_unmap(EDGE + 4096, 1) == handleabi::ERR_NOTMAPPED, "the page after the edge buffer is unmapped");
    u64 ch[2] = {};
    check(sys::call(SYS_CHANNEL_CREATE, reinterpret_cast<u64>(ch)) == 0, "a channel exists for the edge tests");
    straddle("channel_create", SYS_CHANNEL_CREATE, 16, OUT, 0, 0, 0, 0, 0);
    straddle("vmo_create", SYS_VMO_CREATE, 8, 1, 0, OUT, 0, 0, 0);
    straddle("handle_narrow", SYS_HANDLE_NARROW, 8, ch[0], 0xFFFF, OUT, 0, 0, 0);
    straddle("pipe_create", SYS_PIPE_CREATE, 16, 16, OUT, 0, 0, 0, 0);
    straddle("clock", SYS_CLOCK, 32, OUT, 0, 0, 0, 0, 0);
    straddle("file_stat", SYS_FILE_STAT, 32, ch[0], OUT, 0, 0, 0, 0);
    straddle("dir_entry", SYS_DIR_ENTRY, 280, ch[0], 0, OUT, 0, 0, 0);
    straddle("process_status", SYS_PROCESS_STATUS, 8, ch[0], OUT, 0, 0, 0, 0);
    straddle("channel_recv", SYS_CHANNEL_RECV, 16, ch[0], OUT, 16, 0, 0, 0);
    sys::handle_close(ch[0]);
    sys::handle_close(ch[1]);
    sys::vm_unmap(EDGE, 1);
    sys::handle_close(vmo);
}

void bad_pointer_leaks() {
    const u64 bad[] = { 0, 8, 0x400000 + 0x10, USER_LIMIT - 8, 0xFFFF800000001000ull, ~0ull };
    for (u32 i = 0; i < 1500; ++i) {
        u64 p = bad[i % 6];
        call6(SYS_CHANNEL_CREATE, p);
        call6(SYS_VMO_CREATE, 1, 0, p);
        call6(SYS_PIPE_CREATE, 16, p);
        call6(SYS_HANDLE_NARROW, pool_pick(), 0xFFFF, p);
    }
    check(capacity_pairs() == g_capacity_pairs, "bad output pointers must not leak handle slots");
}

void remapper(u64 vmo) {
    while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
        sys::vm_map(vmo, RACE_X, 0, 1, 3);
        sys::yield();
        sys::vm_unmap(RACE_X, 1);
        sys::yield();
    }
    sys::thread_exit(0);
}

void race_remap() {
    u64 vmo = 0;
    check(sys::vmo_create(1, &vmo) == 0, "a memory object can be created for the remap race");
    check(sys::vm_map(vmo, RACE_Y, 0, 1, 3) == static_cast<i64>(RACE_Y), "the stable view maps");
    volatile u64* stable = reinterpret_cast<volatile u64*>(RACE_Y);
    __atomic_store_n(&g_stop, 0u, __ATOMIC_RELEASE);
    i64 tid = sys::thread_create(reinterpret_cast<void (*)(u64)>(&remapper), reinterpret_cast<u64>(g_stacks[0]) + 8192 - 8, vmo);
    check(tid > 0, "the remapping thread starts");
    u64 wrote = 0;
    u64 refused = 0;
    u64 atomic_bad = 0;
    u64 leaked_bad = 0;
    refresh_pool();
    for (u32 i = 0; i < RACE_CALLS; ++i) {
        u64 off = 16 * (i % 64);
        for (u32 w = 0; w < 4; ++w) stable[off / 8 + w] = 0;
        i64 r = call6(SYS_CHANNEL_CREATE, RACE_X + off);
        if (r == 0) {
            ++wrote;
            u64 pair[2] = { stable[off / 8], stable[off / 8 + 1] };
            if (pair[0] == 0 || pair[1] == 0 || sys::handle_close(pair[0]) != 0 || sys::handle_close(pair[1]) != 0) ++leaked_bad;
        } else if (r == ERR_FAULT) {
            ++refused;
            if (stable[off / 8] != 0 || stable[off / 8 + 1] != 0) ++atomic_bad;
        } else {
            ++leaked_bad;
        }
        stable[off / 8] = 0;
        r = call6(SYS_VMO_CREATE, 1, 0, RACE_X + off + 8);
        if (r == 0) {
            u64 h = stable[off / 8 + 1];
            if (h == 0 || sys::handle_close(h) != 0) ++leaked_bad;
        } else if (r != ERR_FAULT) {
            ++leaked_bad;
        }
        stable[off / 8 + 1] = 0;
        r = call6(SYS_HANDLE_NARROW, g_pool[0], 0x3, RACE_X + off + 8);
        if (r == 0) {
            u64 h = stable[off / 8 + 1];
            if (h == 0 || sys::handle_close(h) != 0) ++leaked_bad;
        } else if (r == ERR_FAULT) {
            if (stable[off / 8 + 1] != 0) ++atomic_bad;
        } else {
            ++leaked_bad;
        }
        call6(SYS_CLOCK, RACE_X + off);
        call6(SYS_FILE_STAT, pool_pick(), RACE_X + off);
        sys::yield();
    }
    close_pool();
    __atomic_store_n(&g_stop, 1u, __ATOMIC_RELEASE);
    check(sys::thread_join(static_cast<u64>(tid)) == 0, "the remapping thread ends normally");
    sys::vm_unmap(RACE_Y, 1);
    sys::handle_close(vmo);
    check(leaked_bad == 0, "a copy racing an unmap must either succeed with usable handles or fail with an error");
    check(atomic_bad == 0, "a copy racing an unmap that fails must have written nothing");
    check(capacity_pairs() == g_capacity_pairs, "copies racing unmaps must not leak handle slots");
    Line l;
    l.add("[fuzz] remap race: ");
    l.add(wrote);
    l.add(" channel_create calls wrote their result, ");
    l.add(refused);
    l.add(" were refused");
    l.flush();
}

volatile u64 g_shared[6];

void churner(u64) {
    for (u32 i = 0; i < HANDLE_RACE_CALLS && !__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE); ++i) {
        u32 slot = i % 6;
        u64 old = __atomic_load_n(&g_shared[slot], __ATOMIC_ACQUIRE);
        sys::handle_close(old);
        u64 pair[2] = {};
        if (sys::call(SYS_CHANNEL_CREATE, reinterpret_cast<u64>(pair)) == 0) {
            __atomic_store_n(&g_shared[slot], pair[0], __ATOMIC_RELEASE);
            sys::handle_close(pair[1]);
        } else {
            __atomic_store_n(&g_shared[slot], 0, __ATOMIC_RELEASE);
        }
    }
    sys::thread_exit(0);
}

void race_handles() {
    __atomic_store_n(&g_stop, 0u, __ATOMIC_RELEASE);
    for (u32 i = 0; i < 6; ++i) g_shared[i] = 0;
    i64 tid = sys::thread_create(reinterpret_cast<void (*)(u64)>(&churner), reinterpret_cast<u64>(g_stacks[1]) + 8192 - 8, 0);
    check(tid > 0, "the handle churn thread starts");
    for (u32 i = 0; i < HANDLE_RACE_CALLS; ++i) {
        u64 h = __atomic_load_n(&g_shared[below(6)], __ATOMIC_ACQUIRE);
        u64 other = __atomic_load_n(&g_shared[below(6)], __ATOMIC_ACQUIRE);
        reinterpret_cast<u64*>(g_scratch + 768)[0] = other;
        switch (below(6)) {
        case 0: call6(SYS_HANDLE_INFO, h); break;
        case 1: call6(SYS_HANDLE_NARROW, h, 0x3, scratch_at(0)); break;
        case 2: call6(SYS_CHANNEL_SEND, h, scratch_at(16), 4, scratch_at(768), 1); break;
        case 3: call6(SYS_CHANNEL_RECV, h, scratch_at(64), 64, scratch_at(512), 4, 0); break;
        case 4: {
            u32 entry[4] = { static_cast<u32>(h), static_cast<u32>(h >> 32), 1, 0 };
            __builtin_memcpy(g_scratch + 1024, entry, sizeof(entry));
            call6(SYS_HANDLE_WAIT, scratch_at(1024), 1, 1);
            break;
        }
        default: call6(SYS_HANDLE_CLOSE, h); break;
        }
        close_harvested();
    }
    __atomic_store_n(&g_stop, 1u, __ATOMIC_RELEASE);
    check(sys::thread_join(static_cast<u64>(tid)) == 0, "the handle churn thread ends normally");
    for (u32 i = 0; i < 6; ++i) sys::handle_close(g_shared[i]);
    close_harvested();
    check(capacity_pairs() == g_capacity_pairs, "handles used while another thread closes them must not leak slots");
}

u64 parse_number(const char* s) {
    u64 v = 0;
    for (; *s >= '0' && *s <= '9'; ++s) v = v * 10 + static_cast<u64>(*s - '0');
    return v;
}

}

extern "C" i64 aphelion_main(u64 argument, u64) {
    if (argument != 0) {
        begin::Start start;
        if (!begin::receive(argument, &start)) return 100;
        if (start.parsed.argc > 1) g_seed ^= parse_number(start.parsed.argv[1]) * 0x9E3779B97F4A7C15ull;
    }
    g_capacity_pairs = capacity_pairs();
    check(g_capacity_pairs >= 32, "a fresh process can create at least 32 channels");
    edges();
    atomic_copy();
    bad_pointer_leaks();
    sweep();
    check(capacity_pairs() == g_capacity_pairs, "a sweep of garbage calls must not leak handle slots");
    race_remap();
    race_handles();

    u32 never = 0;
    for (u32 nr = 0; nr < SYS_COUNT; ++nr)
        if (!g_succeeded[nr]) ++never;
    Line l;
    l.add("[fuzz] ");
    l.add(g_calls);
    l.add(" calls, ");
    l.add(g_register_checks);
    l.add(" register checks, ");
    l.add(g_anomalies);
    l.add(" anomalies, ");
    l.add(g_failed_checks);
    l.add(" failed checks, ");
    l.add(static_cast<u64>(SYS_COUNT - never));
    l.add(" of ");
    l.add(static_cast<u64>(SYS_COUNT));
    l.add(" syscalls succeeded at least once");
    l.flush();
    u64 bad = g_anomalies + g_failed_checks;
    return bad == 0 ? 0 : static_cast<i64>(bad < 99 ? bad : 99);
}
