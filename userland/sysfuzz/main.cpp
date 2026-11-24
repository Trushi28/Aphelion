#include <begin.hpp>
#include <cosmos/handleabi.hpp>
#include <cosmos/fileabi.hpp>

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
constexpr u64 THREAD_STACKS = WINDOW + 0x2000000;
constexpr u64 OUT = 0xDEADBEEFCAFEF00Dull;
constexpr i64 ERR_LOWEST = -19;
constexpr u32 SWEEP_CALLS = 6000;
constexpr u32 RACE_CALLS = 1500;
constexpr u32 HANDLE_RACE_CALLS = 2500;
constexpr u32 MAX_REPORTS = 24;
constexpr u64 SWEEP_BUDGET_MS = 6000;
constexpr u64 RACE_BUDGET_MS = 3000;
constexpr u64 PROCESS_WAIT_MS = 10000;
constexpr u32 BUDGET_SWEEP = 1;
constexpr u32 BUDGET_REMAP = 2;
constexpr u32 BUDGET_HANDLES = 4;

constexpr u8 NODE_KIND_FOLDER = 1;
constexpr u64 R_READ = 1;
constexpr u64 R_WRITE = 2;
constexpr u64 R_ENUM = 8;
constexpr u64 R_CREATE = 16;
constexpr u64 R_UNLINK = 32;
constexpr u64 R_DELEGATE = 256;
constexpr u64 R_EXEC = 512;
constexpr u64 R_RENAME = 64;

u64 g_scratch_dir;
u64 g_apps_dir;
u64 g_exit_file;
u64 g_spin_file;
u64 g_p_proc;
bool g_children_in_pool = true;
u64 g_p_vmo;
u64 g_p_chan[4];
u32 g_p_chan_count;

u64 slen(const char* s) {
    u64 n = 0;
    while (s[n]) ++n;
    return n;
}

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
u64 g_user_seed;
u32 g_budget_hit;
u64 g_register_checks;
u64 g_anomalies;
u64 g_failed_checks;
u8 g_succeeded[SYS_COUNT];

alignas(4096) u8 g_scratch[8192];
alignas(4096) u8 g_stacks[2][8192];

u64 g_pool[24];
u32 g_pool_count;
u64 g_stale[16];
u32 g_stale_count;
u64 g_harvest[192];
u32 g_harvest_count;
u64 g_capacity_pairs;
u64 g_p_dir;
u64 g_p_dir_ro;
u64 g_p_file;
u64 g_p_file_ro;

const char* const g_names[] = {
    "a", "b", "f1", "f2", "d1", "d1/x", "d1/y", "d2/deep/file", "pf", "n0", "..", "../sysfuzz-esc", "a/../..", "/sysfuzz-esc",
    "ram:/sysfuzz-esc", ".", "x/", "//", "a//b", "\001bad", "a b", "a:b", "x\\y", "\377\376", "........", "d1/", "d1/..", "e/../../sysfuzz-esc",
};
constexpr u32 NAME_COUNT = sizeof(g_names) / sizeof(g_names[0]);

volatile u32 g_stop;
volatile u32 g_gate;
volatile u32 g_started;
volatile u32 g_passed;

u64 now_ms() {
    userabi::ClockRecord record{};
    sys::clock(&record);
    return record.uptime_ms;
}

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

bool protected_handle(u64 h) { return h != 0 && (h == g_scratch_dir || h == g_apps_dir || h == g_exit_file || h == g_spin_file); }

bool creates_handles(u64 nr) {
    return nr == SYS_HANDLE_NARROW || nr == SYS_CHANNEL_CREATE || nr == SYS_CHANNEL_RECV || nr == SYS_VMO_CREATE || nr == SYS_FILE_OPEN ||
           nr == SYS_FILE_CREATE || nr == SYS_PIPE_CREATE || nr == SYS_PROCESS_CREATE;
}

void keep_handles_visible(u64 nr, u64* a) {
    if (!creates_handles(nr)) return;
    for (u32 k = 0; k < 6; ++k)
        if (a[k] >= WINDOW && a[k] < WINDOW + 0x100000) a[k] = reinterpret_cast<u64>(g_scratch) + 16 * below(32);
}

void keep_protected_out(u64* a) {
    for (u32 k = 0; k < 6; ++k)
        if (protected_handle(a[k])) a[k] = 0x7777777700000003ull;
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
            if (value == 0 || protected_handle(value) || in_pool(value) || g_harvest_count >= sizeof(g_harvest) / sizeof(g_harvest[0])) continue;
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
    g_p_chan_count = 0;
    for (u32 i = 0; i < 2; ++i) {
        if (call6(SYS_CHANNEL_CREATE, reinterpret_cast<u64>(pair)) == 0) {
            g_pool[g_pool_count++] = pair[0];
            g_pool[g_pool_count++] = pair[1];
            g_p_chan[g_p_chan_count++] = pair[0];
            g_p_chan[g_p_chan_count++] = pair[1];
        }
    }
    u64 vmo = 0;
    g_p_vmo = 0;
    if (sys::vmo_create(2, &vmo) == 0) g_p_vmo = g_pool[g_pool_count++] = vmo;
    u64 pipe[2] = {};
    if (sys::pipe_create(16, pipe) == 0) {
        g_pool[g_pool_count++] = pipe[0];
        g_pool[g_pool_count++] = pipe[1];
    }
    g_p_dir = g_p_dir_ro = g_p_file = g_p_file_ro = g_p_proc = 0;
    if (g_spin_file && g_children_in_pool) {
        u64 ph = 0;
        if (call6(SYS_PROCESS_CREATE, g_spin_file, 0, reinterpret_cast<u64>(&ph), 0) == 0) g_p_proc = g_pool[g_pool_count++] = ph;
    }
    if (g_scratch_dir) {
        u64 h = 0;
        if (call6(SYS_HANDLE_NARROW, g_scratch_dir, 0x3FF, reinterpret_cast<u64>(&h)) == 0) g_p_dir = g_pool[g_pool_count++] = h;
        if (call6(SYS_HANDLE_NARROW, g_scratch_dir, 9, reinterpret_cast<u64>(&h)) == 0) g_p_dir_ro = g_pool[g_pool_count++] = h;
        u64 fh = 0;
        i64 r = call6(SYS_FILE_CREATE, g_scratch_dir, reinterpret_cast<u64>("pf"), 2, 3, 0, reinterpret_cast<u64>(&fh));
        if (r == handleabi::ERR_EXISTS) r = call6(SYS_FILE_OPEN, g_scratch_dir, reinterpret_cast<u64>("pf"), 2, 3, reinterpret_cast<u64>(&fh));
        if (r == 0) {
            g_p_file = g_pool[g_pool_count++] = fh;
            if (call6(SYS_HANDLE_NARROW, fh, 1, reinterpret_cast<u64>(&h)) == 0) g_p_file_ro = g_pool[g_pool_count++] = h;
        }
    }
}

u64 pool_pick() { return g_pool_count ? g_pool[below(g_pool_count)] : 0x7777777700000001ull; }

u64 stale_pick() { return g_stale_count ? g_stale[below(g_stale_count < 16 ? g_stale_count : 16)] : 0x7777777700000002ull; }

u64 pool_chan_pick() { return g_p_chan_count ? g_p_chan[below(g_p_chan_count)] : pool_pick(); }

u64 pool_nonchan_pick() {
    for (u32 attempt = 0; attempt < 12; ++attempt) {
        u64 h = pool_pick();
        bool chan = false;
        for (u32 i = 0; i < g_p_chan_count; ++i) chan = chan || g_p_chan[i] == h;
        if (!chan) return h;
    }
    return g_p_vmo ? g_p_vmo : 0x7777777700000001ull;
}

u64 pool_vmo_pick() { return g_p_vmo ? g_p_vmo : pool_pick(); }

u64 pool_dir_pick() {
    u64 c[2];
    u32 n = 0;
    if (g_p_dir) c[n++] = g_p_dir;
    if (g_p_dir_ro) c[n++] = g_p_dir_ro;
    return n ? c[below(n)] : pool_pick();
}

u64 pool_file_pick() {
    u64 c[3];
    u32 n = 0;
    if (g_p_file) c[n++] = g_p_file;
    if (g_p_file_ro) c[n++] = g_p_file_ro;
    if (g_pool_count) c[n++] = g_pool[below(g_pool_count)];
    return n ? c[below(n)] : pool_pick();
}

u64 name_ptr() { return reinterpret_cast<u64>(g_names[below(NAME_COUNT)]); }

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
        if (below(10) < 6) {
            u64* entries = reinterpret_cast<u64*>(g_scratch + 1280);
            u32 count = 1 + static_cast<u32>(below(3));
            for (u32 i = 0; i < count; ++i) {
                entries[2 * i] = pool_pick();
                reinterpret_cast<u32*>(&entries[2 * i + 1])[0] = static_cast<u32>(1 + below(3));
                reinterpret_cast<u32*>(&entries[2 * i + 1])[1] = 0;
            }
            a[0] = scratch_at(1280);
            a[1] = count;
        }
        break;
    case SYS_VMO_SIZE:
        if (below(10) < 7) {
            a[0] = pool_vmo_pick();
            a[1] = scratch_at(8 * below(64));
        }
        break;
    case SYS_CHANNEL_RECV:
        a[5] &= ~1ull;
        if (below(2)) {
            a[0] = pool_chan_pick();
            a[1] = scratch_at(0);
            a[2] = below(300);
            a[3] = scratch_at(512);
            a[4] = below(7);
        }
        break;
    case SYS_CHANNEL_SEND:
        if (below(2)) {
            reinterpret_cast<u64*>(g_scratch + 768)[0] = pool_nonchan_pick();
            reinterpret_cast<u64*>(g_scratch + 768)[1] = pool_nonchan_pick();
            a[0] = pool_chan_pick();
            a[1] = scratch_at(0);
            a[2] = below(8);
            a[3] = scratch_at(768);
            a[4] = below(4);
        } else {
            a[4] = 0;
        }
        break;
    case SYS_VMO_CREATE:
        a[0] = pick(pages, 7);
        if (below(4)) a[1] = 0;
        if (below(2)) a[2] = scratch_at(8 * below(64));
        break;
    case SYS_VM_MAP:
        if (below(10) < 8) {
            a[0] = pool_vmo_pick();
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
    case SYS_FILE_OPEN:
    case SYS_FILE_CREATE:
    case SYS_FILE_UNLINK:
    case SYS_FILE_RENAME:
        if (g_scratch_dir && below(10) < 8) {
            static const u64 rights[] = { 0, 1, 3, 9, 0x7F, 0x17F, 0x3FF, 0x400, 0x200, 2 };
            a[0] = pool_dir_pick();
            a[1] = name_ptr();
            a[2] = below(8) ? slen(reinterpret_cast<const char*>(a[1])) : below(300);
            if (nr == SYS_FILE_OPEN) {
                a[3] = pick(rights, 10);
                a[4] = scratch_at(16 * below(32));
            } else if (nr == SYS_FILE_CREATE) {
                a[3] = pick(rights, 10);
                a[4] = below(3);
                a[5] = scratch_at(16 * below(32));
            } else if (nr == SYS_FILE_UNLINK) {
                a[3] = below(3);
            } else {
                a[3] = name_ptr();
                a[4] = below(8) ? slen(reinterpret_cast<const char*>(a[3])) : below(300);
                a[5] = below(3);
            }
        }
        break;
    case SYS_FILE_READ:
    case SYS_FILE_WRITE:
        if (g_scratch_dir && below(10) < 8) {
            static const u64 offsets[] = { 0, 1, 7, 100, 4096, 5000, 1ull << 20, 1ull << 40, 1ull << 62, ~0ull };
            a[0] = pool_file_pick();
            a[1] = pick(offsets, 10);
            a[2] = scratch_at(2048 + below(512));
            a[3] = below(1300);
        }
        break;
    case SYS_FILE_STAT:
        if (below(10) < 8) {
            a[0] = pool_file_pick();
            a[1] = scratch_at(1024 + 8 * below(16));
        }
        break;
    case SYS_FILE_RESIZE:
        if (g_scratch_dir && below(10) < 8) {
            static const u64 sizes[] = { 0, 1, 100, 4096, 65536, 1ull << 20, 1ull << 40, ~0ull };
            a[0] = pool_file_pick();
            a[1] = pick(sizes, 8);
        }
        break;
    case SYS_PROCESS_CREATE:
        if (g_spin_file && below(10) < 6) {
            a[0] = below(2) ? g_exit_file : pool_pick();
            a[1] = 0;
            a[2] = scratch_at(16 * below(32));
            a[3] = below(4) == 0 ? below(3) : 0;
        }
        break;
    case SYS_PROCESS_STATUS:
    case SYS_PROCESS_KILL:
        if (g_p_proc && below(10) < 7) {
            a[0] = g_p_proc;
            a[1] = scratch_at(1024 + 8 * below(16));
        }
        break;
    case SYS_DIR_ENTRY:
        if (g_scratch_dir && below(10) < 8) {
            a[0] = pool_dir_pick();
            a[1] = below(24);
            a[2] = scratch_at(1536 + 8 * below(16));
        }
        break;
    default:
        break;
    }
}

bool sweepable(u64 nr) { return nr != SYS_EXIT && nr != SYS_THREAD_EXIT && nr != SYS_THREAD_CREATE; }

void sweep() {
    refresh_pool();
    u64 started = now_ms();
    for (u32 i = 0; i < SWEEP_CALLS; ++i) {
        if (i % 64 == 0 && now_ms() - started > SWEEP_BUDGET_MS) {
            g_budget_hit |= BUDGET_SWEEP;
            break;
        }
        if (i % 100 == 0) {
            close_harvested();
            refresh_pool();
        }
        u64 nr = below(40) == 0 ? SYS_COUNT + below(1000) : below(SYS_COUNT);
        if (nr < SYS_COUNT && !sweepable(nr)) continue;
        u64 a[6];
        for (u32 k = 0; k < 6; ++k) a[k] = interesting();
        shape(nr, a);
        keep_protected_out(a);
        keep_handles_visible(nr, a);
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
    g_children_in_pool = false;
    refresh_pool();
    u64 started = now_ms();
    u32 iterations = 0;
    for (u32 i = 0; i < RACE_CALLS; ++i) {
        if (i % 16 == 0 && now_ms() - started > RACE_BUDGET_MS) {
            g_budget_hit |= BUDGET_REMAP;
            break;
        }
        ++iterations;
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
    g_children_in_pool = true;
    __atomic_store_n(&g_stop, 1u, __ATOMIC_RELEASE);
    check(sys::thread_join(static_cast<u64>(tid)) == 0, "the remapping thread ends normally");
    sys::vm_unmap(RACE_Y, 1);
    sys::handle_close(vmo);
    check(leaked_bad == 0, "a copy racing an unmap must either succeed with usable handles or fail with an error");
    check(atomic_bad == 0, "a copy racing an unmap that fails must have written nothing");
    check(capacity_pairs() == g_capacity_pairs, "copies racing unmaps must not leak handle slots");
    Line l;
    l.add("[fuzz] remap race: ");
    l.add(static_cast<u64>(iterations));
    l.add(" rounds, ");
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
    u64 handle_started = now_ms();
    for (u32 i = 0; i < HANDLE_RACE_CALLS; ++i) {
        if (i % 16 == 0 && now_ms() - handle_started > RACE_BUDGET_MS) {
            g_budget_hit |= BUDGET_HANDLES;
            break;
        }
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


i64 f_open(u64 dir, const char* name, u64 rights, u64* out) {
    return call6(SYS_FILE_OPEN, dir, reinterpret_cast<u64>(name), slen(name), rights, reinterpret_cast<u64>(out));
}

i64 f_create(u64 dir, const char* name, u64 rights, u64 flags, u64* out) {
    return call6(SYS_FILE_CREATE, dir, reinterpret_cast<u64>(name), slen(name), rights, flags, reinterpret_cast<u64>(out));
}

i64 f_unlink(u64 dir, const char* name, u64 flags) { return call6(SYS_FILE_UNLINK, dir, reinterpret_cast<u64>(name), slen(name), flags); }

i64 f_rename(u64 dir, const char* from, const char* to, u64 flags) {
    return call6(SYS_FILE_RENAME, dir, reinterpret_cast<u64>(from), slen(from), reinterpret_cast<u64>(to), slen(to), flags);
}

i64 f_write(u64 h, u64 offset, const void* data, u64 n) { return call6(SYS_FILE_WRITE, h, offset, reinterpret_cast<u64>(data), n); }

i64 f_read(u64 h, u64 offset, void* data, u64 n) { return call6(SYS_FILE_READ, h, offset, reinterpret_cast<u64>(data), n); }

i64 f_stat(u64 h, fileabi::Stat* st) { return call6(SYS_FILE_STAT, h, reinterpret_cast<u64>(st)); }

i64 f_resize(u64 h, u64 size) { return call6(SYS_FILE_RESIZE, h, size); }

i64 d_entry(u64 dir, u64 index, fileabi::DirEntry* e) { return call6(SYS_DIR_ENTRY, dir, index, reinterpret_cast<u64>(e)); }

void wipe(u64 dir, u32 depth) {
    for (u32 round = 0; round < 256; ++round) {
        fileabi::DirEntry e;
        if (d_entry(dir, 0, &e) != 0) return;
        bool is_dir = e.kind == fileabi::KIND_DIRECTORY;
        if (is_dir && depth < 6) {
            u64 sub = 0;
            if (f_open(dir, e.name, 0x17F, &sub) == 0) {
                wipe(sub, depth + 1);
                sys::handle_close(sub);
            }
        }
        if (f_unlink(dir, e.name, is_dir ? 1 : 0) != 0) return;
    }
}

bool same_bytes(const u8* a, const u8* b, u64 n) {
    for (u64 i = 0; i < n; ++i)
        if (a[i] != b[i]) return false;
    return true;
}

void files_model() {
    u64 D = g_scratch_dir;
    wipe(D, 0);
    fileabi::DirEntry entry;
    check(d_entry(D, 0, &entry) == fileabi::DIR_END, "the scratch directory starts empty");

    u8 pattern[1024];
    u8 back[1024];
    for (u32 i = 0; i < sizeof(pattern); ++i) pattern[i] = static_cast<u8>(i * 7 + 3);

    u64 f = 0;
    check(f_create(D, "f1", R_READ | R_WRITE, 0, &f) == 0 && f != 0, "a file can be created in the scratch directory");
    u64 dup = 0;
    check(f_create(D, "f1", R_READ | R_WRITE, 0, &dup) == handleabi::ERR_EXISTS && dup == 0, "creating a name that exists must say so and give no handle");
    check(f_write(f, 0, pattern, 700) == 700, "a write of 700 bytes reports 700");
    fileabi::Stat st;
    check(f_stat(f, &st) == 0 && st.kind == fileabi::KIND_FILE && st.size == 700, "stat reports a file of 700 bytes");
    for (u32 i = 0; i < sizeof(back); ++i) back[i] = 0;
    check(f_read(f, 0, back, 1024) == 700 && same_bytes(back, pattern, 700), "the 700 bytes read back are the ones written");
    check(f_read(f, 700, back, 64) == 0, "a read at the end of the file returns zero");
    check(f_read(f, 300, back, 1000) == 400 && same_bytes(back, pattern + 300, 400), "a read from the middle returns the rest");
    check(f_write(f, 700, pattern, 100) == 100, "a write at the end extends the file");
    check(f_stat(f, &st) == 0 && st.size == 800, "the file grew to 800 bytes");
    check(f_resize(f, 100) == 0 && f_stat(f, &st) == 0 && st.size == 100, "shrinking a file sets its size");
    check(f_read(f, 0, back, 1024) == 100 && same_bytes(back, pattern, 100), "the bytes that stay after a shrink are unchanged");
    check(f_resize(f, 0) == 0 && f_stat(f, &st) == 0 && st.size == 0, "a file can be emptied");
    check(f_resize(f, 1ull << 40) < 0, "a resize to a terabyte must be refused");
    check(f_write(f, 1ull << 62, pattern, 1) < 0, "a write at an offset of 2^62 must be refused");
    i64 far_read = f_read(f, 1ull << 62, back, 1);
    check(far_read == 0 || far_read < 0, "a read at an offset of 2^62 must return zero or an error");
    i64 past = f_write(f, 4096, pattern, 8);
    check(past < 0 || (past == 8 && f_stat(f, &st) == 0 && st.size == 4104), "a write past the end either fails or leaves the file exactly that long");
    f_resize(f, 0);

    u64 ro = 0;
    check(f_open(D, "f1", R_READ, &ro) == 0 && ro != 0, "a file can be opened read-only");
    check(f_write(ro, 0, pattern, 4) == handleabi::ERR_DENIED, "a write through a read-only handle must be denied");
    check(f_resize(ro, 10) == handleabi::ERR_DENIED, "a resize through a read-only handle must be denied");
    sys::handle_close(ro);
    sys::handle_close(f);

    u64 narrowed = 0;
    check(call6(SYS_HANDLE_NARROW, D, R_READ | R_ENUM, reinterpret_cast<u64>(&narrowed)) == 0, "the scratch directory can be narrowed");
    u64 junk = 0;
    check(f_open(narrowed, "f1", R_WRITE, &junk) == handleabi::ERR_DENIED, "a file cannot be opened with rights the directory lacks");
    check(f_create(narrowed, "zz", R_READ, 0, &junk) == handleabi::ERR_DENIED, "a directory without the create right cannot create");
    check(f_unlink(narrowed, "f1", 0) == handleabi::ERR_DENIED, "a directory without the unlink right cannot unlink");
    check(f_rename(narrowed, "f1", "f9", 0) == handleabi::ERR_DENIED, "a directory without the rename right cannot rename");
    check(f_open(narrowed, "f1", R_READ, &junk) == 0, "a narrowed directory still opens files for reading");
    sys::handle_close(junk);
    sys::handle_close(narrowed);

    check(f_create(D, "d1", 0, fileabi::CREATE_DIRECTORY, &junk) == 0, "a directory can be created");
    u64 inner = 0;
    check(f_create(D, "d1/x", R_READ | R_WRITE, 0, &inner) == 0, "a file can be created inside it");
    sys::handle_close(inner);
    check(f_unlink(D, "d1", 0) == fileabi::ERR_WRONGTYPE, "unlinking a directory as a file must be refused");
    check(f_unlink(D, "d1/x", fileabi::UNLINK_DIRECTORY) == fileabi::ERR_WRONGTYPE, "unlinking a file as a directory must be refused");
    check(f_unlink(D, "d1", fileabi::UNLINK_DIRECTORY) == fileabi::ERR_NOTEMPTY, "unlinking a directory that is not empty must say so");
    check(f_unlink(D, "d1/x", 0) == 0, "a file inside a directory can be unlinked");
    check(f_unlink(D, "d1", fileabi::UNLINK_DIRECTORY) == 0, "an empty directory can be unlinked");
    check(f_open(D, "d1", R_READ, &junk) == fileabi::ERR_NOTFOUND, "a removed directory is not found");
    check(f_open(D, "nope", R_READ, &junk) == fileabi::ERR_NOTFOUND, "a name that was never made is not found");

    u64 g = 0;
    check(f_rename(D, "f1", "f2", 0) == 0, "a file can be renamed");
    check(f_open(D, "f1", R_READ, &junk) == fileabi::ERR_NOTFOUND, "the old name is gone after a rename");
    check(f_create(D, "f3", R_READ, 0, &g) == 0, "a second file can be created");
    sys::handle_close(g);
    check(f_rename(D, "f2", "f3", 0) == handleabi::ERR_EXISTS, "a rename onto an existing name must say so");
    check(f_rename(D, "f2", "f3", fileabi::RENAME_REPLACE) == 0, "a rename with replace overwrites");
    check(f_open(D, "f2", R_READ, &junk) == fileabi::ERR_NOTFOUND, "the replaced source is gone");
    check(f_rename(D, "missing", "f4", 0) == fileabi::ERR_NOTFOUND, "renaming something that is not there must say so");
    wipe(D, 0);

    static const char* const escapes[] = { "..", "../sysfuzz-esc", "a/../../sysfuzz-esc", "/sysfuzz-esc", "ram:/sysfuzz-esc", "./../sysfuzz-esc", "x/..", "sub/../../sysfuzz-esc" };
    for (const char* name : escapes) {
        u64 out = 0;
        Line l;
        l.add("a path that leaves the directory must be refused: ");
        l.add(name);
        bool refused = f_create(D, name, R_READ | R_WRITE, 0, &out) < 0 && f_open(D, name, R_READ, &out) < 0 &&
                       f_create(D, name, 0, fileabi::CREATE_DIRECTORY, &out) < 0 && f_unlink(D, name, 0) < 0;
        check(refused, l.c_str());
    }
    u64 out = 0;
    check(call6(SYS_FILE_OPEN, D, reinterpret_cast<u64>("x"), 0, R_READ, reinterpret_cast<u64>(&out)) == ERR_INVAL, "an empty path must be refused");
    check(call6(SYS_FILE_OPEN, D, reinterpret_cast<u64>("x"), 256, R_READ, reinterpret_cast<u64>(&out)) == ERR_INVAL, "a path of 256 bytes must be refused");
    check(call6(SYS_FILE_OPEN, D, reinterpret_cast<u64>("a\0b"), 3, R_READ, reinterpret_cast<u64>(&out)) == ERR_INVAL, "a path with a NUL in it must be refused");
    check(call6(SYS_FILE_OPEN, D, reinterpret_cast<u64>("x"), 1, 0x400, reinterpret_cast<u64>(&out)) == ERR_INVAL, "rights outside the defined bits must be refused");
    check(call6(SYS_FILE_CREATE, D, reinterpret_cast<u64>("x"), 1, R_READ, 2, reinterpret_cast<u64>(&out)) == ERR_INVAL, "unknown create flags must be refused");
    check(call6(SYS_FILE_UNLINK, D, reinterpret_cast<u64>("x"), 1, 2) == ERR_INVAL, "unknown unlink flags must be refused");
    check(call6(SYS_FILE_RENAME, D, reinterpret_cast<u64>("x"), 1, reinterpret_cast<u64>("y"), 1, 2) == ERR_INVAL, "unknown rename flags must be refused");

    static const char* const names[] = { "n0", "n1", "n2", "n3", "n4", "n5" };
    for (const char* name : names) {
        u64 h = 0;
        check(f_create(D, name, R_READ, 0, &h) == 0, "listing setup: a file is created");
        sys::handle_close(h);
    }
    u32 found[6] = {};
    u32 total = 0;
    bool names_ok = true;
    for (u64 i = 0; i < 16; ++i) {
        i64 r = d_entry(D, i, &entry);
        if (r != 0) {
            check(r == fileabi::DIR_END, "past the last entry the listing must say end");
            break;
        }
        ++total;
        if (entry.name_len != slen(entry.name) || entry.kind != fileabi::KIND_FILE) names_ok = false;
        for (u32 k = 0; k < 6; ++k)
            if (slen(names[k]) == entry.name_len && same_bytes(reinterpret_cast<const u8*>(names[k]), reinterpret_cast<const u8*>(entry.name), entry.name_len)) ++found[k];
    }
    check(total == 6 && names_ok, "the listing has exactly the six files, with consistent names and kinds");
    bool each_once = true;
    for (u32 k = 0; k < 6; ++k) each_once = each_once && found[k] == 1;
    check(each_once, "every created name is listed exactly once");
    check(d_entry(D, ~0ull, &entry) == fileabi::DIR_END && d_entry(D, 1ull << 40, &entry) == fileabi::DIR_END, "a huge index must report the end of the listing");
    wipe(D, 0);
    check(d_entry(D, 0, &entry) == fileabi::DIR_END, "the scratch directory can be emptied again");
}

void pipe_model() {
    u64 pr[2] = {};
    check(call6(SYS_PIPE_CREATE, 0, reinterpret_cast<u64>(pr)) == ERR_INVAL, "a pipe of zero bytes must be refused");
    check(call6(SYS_PIPE_CREATE, fileabi::MAX_PIPE_BYTES + 1, reinterpret_cast<u64>(pr)) == ERR_INVAL, "a pipe past the limit must be refused");
    check(call6(SYS_PIPE_CREATE, ~0ull, reinterpret_cast<u64>(pr)) == ERR_INVAL, "a pipe of 2^64-1 bytes must be refused");
    check(call6(SYS_PIPE_CREATE, 16, reinterpret_cast<u64>(pr)) == 0 && pr[0] != 0 && pr[1] != 0, "a pipe of 16 bytes can be created");
    u8 data[64];
    u8 back[64];
    for (u32 i = 0; i < sizeof(data); ++i) data[i] = static_cast<u8>(i * 5 + 1);
    fileabi::Stat st;
    check(f_stat(pr[0], &st) == 0 && st.kind == fileabi::KIND_STREAM, "a pipe end reports itself as a stream");
    check(f_read(pr[0], 0, back, 8) == handleabi::ERR_AGAIN, "an empty pipe must report would-block, not end of file, while a writer exists");
    check(f_write(pr[0], 0, data, 4) == handleabi::ERR_DENIED, "writing to the read end must be denied");
    check(f_read(pr[1], 0, back, 4) == handleabi::ERR_DENIED, "reading from the write end must be denied");
    u64 cap = 0;
    i64 last = 0;
    while (cap <= fileabi::MAX_PIPE_BYTES) {
        last = f_write(pr[1], 0, data, 1);
        if (last != 1) break;
        ++cap;
    }
    check(last == handleabi::ERR_AGAIN, "filling a pipe one byte at a time must end in would-block");
    check(cap >= 16 && cap <= fileabi::MAX_PIPE_BYTES, "a pipe holds at least the bytes asked for and no more than the maximum");
    u64 drained = 0;
    bool drained_ok = true;
    for (;;) {
        i64 got = f_read(pr[0], 0, back, 64);
        if (got <= 0) {
            check(got == handleabi::ERR_AGAIN, "draining a pipe must end in would-block while the writer exists");
            break;
        }
        for (i64 k = 0; k < got; ++k) drained_ok = drained_ok && back[k] == data[0];
        drained += static_cast<u64>(got);
    }
    check(drained == cap && drained_ok, "a full pipe gives back exactly the bytes it took, unchanged");

    u64 sent = 0;
    u64 received = 0;
    u32 bad_order = 0;
    u32 over = 0;
    u32 would_block_wrong = 0;
    u8 chunk[32];
    for (u32 round = 0; round < 3000; ++round) {
        u64 inflight = sent - received;
        if (below(2)) {
            u64 want = 1 + below(32);
            for (u32 k = 0; k < want; ++k) chunk[k] = static_cast<u8>((sent + k) * 3 + 7);
            i64 w = f_write(pr[1], 0, chunk, want);
            if (w > 0) {
                if (static_cast<u64>(w) > want || inflight + static_cast<u64>(w) > cap) ++over;
                sent += static_cast<u64>(w);
            } else if (w == handleabi::ERR_AGAIN) {
                if (inflight < cap && want <= cap - inflight) ++would_block_wrong;
            } else {
                ++over;
            }
        } else {
            u64 want = 1 + below(32);
            i64 r = f_read(pr[0], 0, back, want);
            if (r > 0) {
                if (static_cast<u64>(r) > want || static_cast<u64>(r) > inflight) {
                    ++over;
                } else {
                    for (i64 k = 0; k < r; ++k)
                        if (back[k] != static_cast<u8>((received + static_cast<u64>(k)) * 3 + 7)) ++bad_order;
                    received += static_cast<u64>(r);
                }
            } else if (r == handleabi::ERR_AGAIN) {
                if (inflight != 0) ++would_block_wrong;
            } else {
                ++over;
            }
        }
    }
    check(bad_order == 0, "3,000 random reads and writes on a pipe: the bytes come out in the order they went in");
    check(over == 0, "3,000 random reads and writes on a pipe: never more out than in, never more in than it holds, no odd results");
    check(would_block_wrong == 0, "3,000 random reads and writes on a pipe: would-block only when it is empty or has no room");

    u64 left = sent - received;
    check(sys::handle_close(pr[1]) == 0, "the write end can be closed");
    i64 r = f_read(pr[0], 0, back, 64);
    bool tail_ok = r == static_cast<i64>(left);
    for (i64 k = 0; tail_ok && k < r; ++k) tail_ok = back[k] == static_cast<u8>((received + static_cast<u64>(k)) * 3 + 7);
    check(tail_ok, "after the writer closes, the buffered bytes can still be read, in order");
    check(f_read(pr[0], 0, back, 64) == 0, "once drained, a pipe whose writer is gone reads as end of file");
    sys::handle_close(pr[0]);

    check(call6(SYS_PIPE_CREATE, 16, reinterpret_cast<u64>(pr)) == 0, "a second pipe can be created");
    sys::handle_close(pr[0]);
    check(f_write(pr[1], 0, data, 4) == handleabi::ERR_CLOSED, "writing to a pipe whose reader is gone must say closed");
    sys::handle_close(pr[1]);
}

void files_consistency() {
    u64 D = g_scratch_dir;
    u32 listed = 0;
    bool sane = true;
    bool reopened = true;
    bool ended = false;
    for (u64 i = 0; i < 512; ++i) {
        fileabi::DirEntry e;
        i64 r = d_entry(D, i, &e);
        if (r != 0) {
            ended = r == fileabi::DIR_END;
            break;
        }
        ++listed;
        if (e.name_len == 0 || e.name_len != slen(e.name)) {
            sane = false;
            continue;
        }
        u64 h = 0;
        if (f_open(D, e.name, R_READ, &h) != 0) {
            reopened = false;
            continue;
        }
        fileabi::Stat st;
        if (f_stat(h, &st) != 0 || st.kind != (e.kind == fileabi::KIND_DIRECTORY ? fileabi::KIND_DIRECTORY : fileabi::KIND_FILE)) sane = false;
        sys::handle_close(h);
    }
    check(ended, "after the sweep the scratch directory listing must end");
    check(sane, "after the sweep every listed name is non-empty, its length matches, and stat agrees with the listing");
    check(reopened, "after the sweep every listed name can be opened again");
    wipe(D, 0);
    fileabi::DirEntry e;
    check(d_entry(D, 0, &e) == fileabi::DIR_END, "after the sweep the scratch directory can be emptied");
    Line l;
    l.add("[fuzz] scratch directory after the sweep held ");
    l.add(static_cast<u64>(listed));
    l.add(" entries");
    l.flush();
}

bool process_exit_status(u64 h, i64* status) {
    u64 started = now_ms();
    while (now_ms() - started < PROCESS_WAIT_MS) {
        i64 st = 0;
        i64 r = call6(SYS_PROCESS_STATUS, h, reinterpret_cast<u64>(&st));
        if (r == 0) {
            *status = st;
            return true;
        }
        if (r != handleabi::ERR_AGAIN) return false;
        sys::yield();
    }
    return false;
}

void open_programs() {
    check(f_open(g_apps_dir, "exit42", R_READ | R_EXEC, &g_exit_file) == 0 && g_exit_file != 0, "the apps directory holds a program that exits with 42");
    check(f_open(g_apps_dir, "spin", R_READ | R_EXEC, &g_spin_file) == 0 && g_spin_file != 0, "the apps directory holds a program that never exits");
}

void process_model() {
    if (!g_exit_file || !g_spin_file) return;
    u64 SENT = 0xAAAA;
    u64 h = SENT;
    check(call6(SYS_PROCESS_CREATE, g_exit_file, 0, reinterpret_cast<u64>(&h), 1) == ERR_INVAL && (h == 0 || h == SENT), "process_create with flags must be refused and give no handle");
    u64 plain = 0;
    check(f_open(g_apps_dir, "exit42", R_READ, &plain) == 0, "a program can be opened without the execute right");
    h = SENT;
    check(call6(SYS_PROCESS_CREATE, plain, 0, reinterpret_cast<u64>(&h), 0) == handleabi::ERR_DENIED && h == 0, "a file without the execute right must not start");
    sys::handle_close(plain);
    h = SENT;
    check(call6(SYS_PROCESS_CREATE, g_apps_dir, 0, reinterpret_cast<u64>(&h), 0) < 0 && h == 0, "a directory is not a program");
    h = SENT;
    check(call6(SYS_PROCESS_CREATE, 0x7777777700000001ull, 0, reinterpret_cast<u64>(&h), 0) == handleabi::ERR_BADHANDLE && h == 0, "a handle that does not exist is not a program");

    u64 nodel = 0;
    check(call6(SYS_HANDLE_NARROW, g_apps_dir, R_READ | R_ENUM, reinterpret_cast<u64>(&nodel)) == 0, "the apps directory can be narrowed");
    h = SENT;
    check(call6(SYS_PROCESS_CREATE, g_exit_file, nodel, reinterpret_cast<u64>(&h), 0) == handleabi::ERR_DENIED && h == 0, "an argument handle without the delegate right must be refused");
    check(sys::call(SYS_HANDLE_INFO, nodel) >= 0, "a refused argument handle stays with the caller");
    sys::handle_close(nodel);

    u64 del = 0;
    check(call6(SYS_HANDLE_NARROW, g_apps_dir, R_READ | R_ENUM | R_DELEGATE, reinterpret_cast<u64>(&del)) == 0, "a delegable narrowed handle can be made");
    h = 0;
    check(call6(SYS_PROCESS_CREATE, g_exit_file, del, reinterpret_cast<u64>(&h), 0) == 0 && h != 0, "a program can be started with an argument handle");
    check(sys::call(SYS_HANDLE_INFO, del) == handleabi::ERR_BADHANDLE, "a delegated argument handle leaves the caller");
    i64 st = 0;
    check(process_exit_status(h, &st) && st == 42, "a child that exits with 42 reports 42");
    i64 again = 0;
    check(call6(SYS_PROCESS_STATUS, h, reinterpret_cast<u64>(&again)) == 0 && again == 42, "a finished child keeps reporting its status");
    check(call6(SYS_PROCESS_KILL, h) == handleabi::ERR_CLOSED, "killing a process that has ended must say closed");
    sys::handle_close(h);

    u64 ch[2] = {};
    check(call6(SYS_CHANNEL_CREATE, reinterpret_cast<u64>(ch)) == 0, "a channel exists for the type checks");
    i64 junk = 0;
    check(call6(SYS_PROCESS_STATUS, ch[0], reinterpret_cast<u64>(&junk)) == ERR_INVAL, "process_status on a channel must be refused");
    check(call6(SYS_PROCESS_KILL, ch[0]) == ERR_INVAL, "process_kill on a channel must be refused");
    sys::handle_close(ch[0]);
    sys::handle_close(ch[1]);

    u64 spin = 0;
    check(call6(SYS_PROCESS_CREATE, g_spin_file, 0, reinterpret_cast<u64>(&spin), 0) == 0 && spin != 0, "a program that never exits can be started");
    i64 running = 0;
    check(call6(SYS_PROCESS_STATUS, spin, reinterpret_cast<u64>(&running)) == handleabi::ERR_AGAIN, "a running child reports would-block");
    u64 only_read = 0;
    u64 only_write = 0;
    check(call6(SYS_HANDLE_NARROW, spin, R_READ, reinterpret_cast<u64>(&only_read)) == 0 && call6(SYS_HANDLE_NARROW, spin, R_WRITE, reinterpret_cast<u64>(&only_write)) == 0, "a process handle can be narrowed");
    check(call6(SYS_PROCESS_KILL, only_read) == handleabi::ERR_DENIED, "a process handle without the write right cannot kill");
    check(call6(SYS_PROCESS_STATUS, only_write, reinterpret_cast<u64>(&running)) == handleabi::ERR_DENIED, "a process handle without the read right cannot ask for the status");
    check(call6(SYS_PROCESS_STATUS, spin, reinterpret_cast<u64>(&running)) == handleabi::ERR_AGAIN, "a refused kill leaves the child running");
    check(call6(SYS_PROCESS_KILL, only_write) == 0, "a handle with the write right can kill");
    i64 killed = 0;
    check(process_exit_status(spin, &killed) && killed == STATUS_KILLED, "a killed child reports the killed status");
    check(call6(SYS_PROCESS_KILL, spin) == handleabi::ERR_CLOSED, "a second kill must say closed");
    sys::handle_close(only_read);
    sys::handle_close(only_write);
    sys::handle_close(spin);

    u64 hs[24];
    u32 made = 0;
    i64 last = 0;
    while (made < 24) {
        u64 hh = 0;
        last = call6(SYS_PROCESS_CREATE, g_spin_file, 0, reinterpret_cast<u64>(&hh), 0);
        if (last != 0) {
            check(hh == 0, "a refused process_create gives no handle");
            break;
        }
        hs[made++] = hh;
    }
    check(made >= 1 && made <= 15 && last == ERR_LIMIT, "starting children until there is no room ends with the limit error after at most 15");
    for (u32 i = 0; i < made; ++i) sys::handle_close(hs[i]);
    bool restarted = false;
    u64 restart_started = now_ms();
    while (!restarted && now_ms() - restart_started < PROCESS_WAIT_MS) {
        u64 hh = 0;
        if (call6(SYS_PROCESS_CREATE, g_spin_file, 0, reinterpret_cast<u64>(&hh), 0) == 0) {
            restarted = true;
            sys::handle_close(hh);
        } else {
            sys::yield();
        }
    }
    check(restarted, "after the children are closed, new ones can be started");
    Line l;
    l.add("[fuzz] started ");
    l.add(static_cast<u64>(made));
    l.add(" children before the limit");
    l.flush();
}

void gate_worker(u64 index) {
    __atomic_add_fetch(&g_started, 1u, __ATOMIC_ACQ_REL);
    while (__atomic_load_n(&g_gate, __ATOMIC_ACQUIRE) == 0) sys::futex_wait(&g_gate, 0, FUTEX_FOREVER);
    __atomic_add_fetch(&g_passed, 1u, __ATOMIC_ACQ_REL);
    sys::thread_exit(static_cast<i64>(100 + index));
}

void quick_worker(u64 index) { sys::thread_exit(static_cast<i64>(200 + index)); }

u64 thread_stack_top(u64 slot) { return THREAD_STACKS + (slot + 1) * 4096 - 8; }

bool wait_for_count(volatile u32* counter, u32 want) {
    u64 started = now_ms();
    while (__atomic_load_n(counter, __ATOMIC_ACQUIRE) < want) {
        if (now_ms() - started > PROCESS_WAIT_MS) return false;
        sys::yield();
    }
    return true;
}

i32 thread_limits() {
    u64 vmo = 0;
    check(sys::vmo_create(16, &vmo) == 0, "a memory object for thread stacks can be created");
    check(sys::vm_map(vmo, THREAD_STACKS, 0, 16, 3) == static_cast<i64>(THREAD_STACKS), "the thread stacks map");
    u64 tids[16] = {};
    u32 made = 0;
    i64 last = 0;
    while (made < 16) {
        last = sys::thread_create(reinterpret_cast<void (*)(u64)>(&gate_worker), thread_stack_top(made), made);
        if (last <= 0) break;
        tids[made++] = static_cast<u64>(last);
    }
    check(last == ERR_LIMIT && made == MAX_LIVE_THREADS - 1, "threads that stay alive stop at 8 including the first, and the next create says limit");
    check(wait_for_count(&g_started, made), "every thread that was created starts");
    check(sys::call(SYS_FUTEX_WAKE, reinterpret_cast<u64>(&g_gate), 0) == 0, "futex_wake of zero waiters returns zero");
    for (u32 i = 0; i < 200; ++i) sys::yield();
    check(__atomic_load_n(&g_passed, __ATOMIC_ACQUIRE) == 0, "a wake while the word is unchanged lets nobody through the gate");
    check(sys::call(SYS_FUTEX_WAKE, reinterpret_cast<u64>(&g_gate), 1) == 0, "a wake of one returns zero");
    for (u32 i = 0; i < 200; ++i) sys::yield();
    check(__atomic_load_n(&g_passed, __ATOMIC_ACQUIRE) == 0, "a spurious wake sends every sleeper back to sleep while the word is unchanged");
    __atomic_store_n(&g_gate, 1u, __ATOMIC_RELEASE);
    sys::call(SYS_FUTEX_WAKE, reinterpret_cast<u64>(&g_gate), 16);
    bool statuses = true;
    for (u32 i = 0; i < made; ++i) statuses = statuses && sys::thread_join(tids[i]) == static_cast<i64>(100 + i);
    check(statuses, "each joined thread reports the status it ended with");
    check(__atomic_load_n(&g_passed, __ATOMIC_ACQUIRE) == made, "every sleeper got through once the word changed");

    u32 more = 0;
    bool quick = true;
    last = 0;
    while (made + more < 16) {
        last = sys::thread_create(reinterpret_cast<void (*)(u64)>(&quick_worker), thread_stack_top(made + more), made + more);
        if (last <= 0) break;
        quick = quick && sys::thread_join(static_cast<u64>(last)) == static_cast<i64>(200 + made + more);
        ++more;
    }
    check(made + more == MAX_THREADS_TOTAL - 1 && last == ERR_LIMIT, "with each thread joined before the next, creation stops at 16 over the life of the process");
    check(quick, "the second batch reports its statuses too");
    check(sys::thread_create(reinterpret_cast<void (*)(u64)>(&quick_worker), thread_stack_top(0), 0) == ERR_LIMIT, "a finished thread's slot is not reused");
    sys::vm_unmap(THREAD_STACKS, 16);
    sys::handle_close(vmo);
    u64 failed = g_failed_checks + g_anomalies;
    return static_cast<i32>(failed < 99 ? failed : 99);
}

void thread_limits_in_child() {
    if (!g_apps_dir) return;
    u64 self = 0;
    check(f_open(g_apps_dir, "sysfuzz", R_READ | R_EXEC, &self) == 0, "the apps directory holds the fuzzer itself");
    if (!self) return;
    u64 h = 0;
    i64 created = ERR_LIMIT;
    u64 started = now_ms();
    while (now_ms() - started < PROCESS_WAIT_MS) {
        h = 0;
        created = call6(SYS_PROCESS_CREATE, self, 0, reinterpret_cast<u64>(&h), 0);
        if (created != ERR_LIMIT) break;
        sys::yield();
    }
    check(created == 0 && h != 0, "a second instance of the fuzzer can be started with no handles once the earlier children are reclaimed");
    i64 st = -1;
    check(process_exit_status(h, &st) && st == 0, "the second instance, which exercises the thread and futex limits, ends with status 0");
    sys::handle_close(h);
    sys::handle_close(self);
}

u64 parse_number(const char* s) {
    u64 v = 0;
    for (; *s >= '0' && *s <= '9'; ++s) v = v * 10 + static_cast<u64>(*s - '0');
    return v;
}

}

void read_seed() {
    u64 h = 0;
    if (f_open(g_scratch_dir, "seed", R_READ, &h) != 0) return;
    char text[32] = {};
    i64 n = f_read(h, 0, text, 31);
    sys::handle_close(h);
    if (n <= 0) return;
    g_user_seed = parse_number(text);
    g_seed ^= g_user_seed * 0x9E3779B97F4A7C15ull;
}

extern "C" i64 aphelion_main(u64 argument, u64 second) {
    if (argument == 0 && second == 0) return thread_limits();
    if (argument != 0) {
        i64 info = sys::call(SYS_HANDLE_INFO, argument);
        if (info >= 0 && handleabi::info_kind(static_cast<u64>(info)) == NODE_KIND_FOLDER) {
            g_scratch_dir = argument;
            g_apps_dir = second;
            read_seed();
        } else {
            begin::Start start;
            if (!begin::receive(argument, &start)) return 100;
            if (start.parsed.argc > 1) {
                g_user_seed = parse_number(start.parsed.argv[1]);
                g_seed ^= g_user_seed * 0x9E3779B97F4A7C15ull;
            }
        }
    }
    if (g_apps_dir) open_programs();
    g_capacity_pairs = capacity_pairs();
    check(g_capacity_pairs >= 32, "a fresh process can create at least 32 channels");
    edges();
    atomic_copy();
    bad_pointer_leaks();
    sweep();
    if (g_scratch_dir) files_consistency();
    check(capacity_pairs() == g_capacity_pairs, "a sweep of garbage calls must not leak handle slots");
    race_remap();
    race_handles();
    if (g_scratch_dir) files_model();
    pipe_model();
    if (g_apps_dir) {
        process_model();
        thread_limits_in_child();
    }

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
    l.add(" syscalls succeeded at least once, seed ");
    l.add(g_user_seed);
    l.flush();
    if (g_budget_hit) {
        Line budget;
        budget.add("[fuzz] time budget reached, fewer rounds than planned in:");
        if (g_budget_hit & BUDGET_SWEEP) budget.add(" sweep");
        if (g_budget_hit & BUDGET_REMAP) budget.add(" remap-race");
        if (g_budget_hit & BUDGET_HANDLES) budget.add(" handle-race");
        budget.flush();
    }
    Line never_line;
    never_line.add("[fuzz] never succeeded:");
    for (u32 nr = 0; nr < SYS_COUNT; ++nr) {
        if (g_succeeded[nr]) continue;
        never_line.add(" ");
        never_line.add(static_cast<u64>(nr));
    }
    never_line.flush();
    u64 bad = g_anomalies + g_failed_checks;
    return bad == 0 ? 0 : static_cast<i64>(bad < 99 ? bad : 99);
}
