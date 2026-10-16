#include <cosmos/percore.hpp>
#include <cosmos/cpu.hpp>

namespace percore {

constexpr u32 MSR_EFER = 0xC0000080;
constexpr u32 MSR_PAT = 0x277;
constexpr u64 EFER_NXE = 1ull << 11;
constexpr u64 CR0_WP = 1ull << 16;

struct Seen {
    bool nx_supported;
    bool pat_supported;
    u64 efer;
    u64 cr0;
    u64 pat;
};

static Seen g_seen[MAX_CORES];
static volatile u32 g_next_slot = 0;
static volatile u64 g_done_mask = 0;
static volatile bool g_nx_on = false;
static volatile bool g_wc_on = false;

void early_init() {
    u32 slot = __atomic_fetch_add(&g_next_slot, 1u, __ATOMIC_SEQ_CST);
    if (slot >= MAX_CORES) cpu::hang();

    u32 a, b, c, d;
    cpu::cpuid(0x80000000u, 0, &a, &b, &c, &d);
    bool nx = false;
    if (a >= 0x80000001u) {
        cpu::cpuid(0x80000001u, 0, &a, &b, &c, &d);
        nx = (d & (1u << 20)) != 0;
    }
    cpu::cpuid(1, 0, &a, &b, &c, &d);
    bool pat = (d & (1u << 16)) != 0;

    if (nx) cpu::wrmsr(MSR_EFER, cpu::rdmsr(MSR_EFER) | EFER_NXE);
    cpu::write_cr0(cpu::read_cr0() | CR0_WP);
    if (pat) cpu::wrmsr(MSR_PAT, PAT_VALUE);

    Seen& s = g_seen[slot];
    s.nx_supported = nx;
    s.pat_supported = pat;
    s.efer = cpu::rdmsr(MSR_EFER);
    s.cr0 = cpu::read_cr0();
    s.pat = pat ? cpu::rdmsr(MSR_PAT) : 0;

    if (slot == 0) {
        g_nx_on = nx;
        g_wc_on = pat;
    }
    __atomic_or_fetch(&g_done_mask, 1ull << slot, __ATOMIC_SEQ_CST);
}

bool nx_enabled() { return g_nx_on; }
bool wc_enabled() { return g_wc_on; }

u32 cores_seen() {
    u64 m = __atomic_load_n(&g_done_mask, __ATOMIC_SEQ_CST);
    u32 n = 0;
    while (m) { n += static_cast<u32>(m & 1); m >>= 1; }
    return n;
}

bool selftest() {
    u64 mask = __atomic_load_n(&g_done_mask, __ATOMIC_SEQ_CST);
    if (!(mask & 1)) return false;
    const Seen& bsp = g_seen[0];
    for (u32 i = 0; i < MAX_CORES; ++i) {
        if (!(mask & (1ull << i))) continue;
        const Seen& s = g_seen[i];
        if (s.nx_supported != bsp.nx_supported || s.pat_supported != bsp.pat_supported) return false;
        if (!(s.cr0 & CR0_WP)) return false;
        if (s.nx_supported && !(s.efer & EFER_NXE)) return false;
        if (s.pat_supported && s.pat != PAT_VALUE) return false;
    }
    return true;
}

}
