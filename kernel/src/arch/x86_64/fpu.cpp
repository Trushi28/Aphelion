#include <cosmos/fpu.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/serial.hpp>

extern "C" u64 fpu_probe(u64 test_avx);
extern "C" void fpu_load_regs(const void* buf, u64 avx);
extern "C" void fpu_store_regs(void* buf, u64 avx);
extern "C" void fpu_set_modes(u64 control_word, u64 mxcsr);
extern "C" void fpu_get_modes(u16* control_word, u32* mxcsr);

namespace fpu {

constexpr u64 CR0_MP = 1ull << 1;
constexpr u64 CR0_EM = 1ull << 2;
constexpr u64 CR0_TS = 1ull << 3;
constexpr u64 CR0_NE = 1ull << 5;
constexpr u64 CR4_OSFXSR = 1ull << 9;
constexpr u64 CR4_OSXMMEXCPT = 1ull << 10;
constexpr u64 CR4_OSXSAVE = 1ull << 18;
constexpr u64 XCR0_X87 = 1ull << 0;
constexpr u64 XCR0_SSE = 1ull << 1;
constexpr u64 XCR0_AVX = 1ull << 2;

constexpr u64 PROBE_X87 = 1;
constexpr u64 PROBE_SSE2 = 2;
constexpr u64 PROBE_AVX = 4;

struct Seen {
    bool sse2;
    bool xsave;
    bool avx;
    bool avx2;
    bool avx512f;
    u64 xcr0;
    u64 probe;
};

static Seen g_seen[MAX_CORES];
static u32 g_state_bytes = 0;
static bool g_xsave = false;
static bool g_avx_state = false;
static u64 g_xcr0 = 0;
static volatile u32 g_next_slot = 0;
static volatile u64 g_done_mask = 0;

void init_core() {
    if (!ENABLED) return;
    u32 slot = __atomic_fetch_add(&g_next_slot, 1u, __ATOMIC_SEQ_CST);
    if (slot >= MAX_CORES) cpu::hang();

    u32 a, b, c, d;
    cpu::cpuid(1, 0, &a, &b, &c, &d);
    bool fxsr = (d & (1u << 24)) != 0;
    bool sse2 = (d & (1u << 26)) != 0;
    bool xsave = (c & (1u << 26)) != 0;
    bool avx = (c & (1u << 28)) != 0;
    cpu::cpuid(7, 0, &a, &b, &c, &d);
    bool avx2 = (b & (1u << 5)) != 0;
    bool avx512f = (b & (1u << 16)) != 0;

    Seen& s = g_seen[slot];
    s = {};
    s.sse2 = fxsr && sse2;
    s.xsave = xsave;
    s.avx = avx;
    s.avx2 = avx2;
    s.avx512f = avx512f;

    if (s.sse2) {
        u64 cr0 = cpu::read_cr0();
        cr0 &= ~(CR0_EM | CR0_TS);
        cr0 |= CR0_MP | CR0_NE;
        cpu::write_cr0(cr0);

        u64 cr4 = cpu::read_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT;
        if (xsave) cr4 |= CR4_OSXSAVE;
        cpu::write_cr4(cr4);

        if (xsave) {
            cpu::cpuid(0xD, 0, &a, &b, &c, &d);
            u64 supported = a;
            u64 want = XCR0_X87 | XCR0_SSE;
            if (avx && (supported & XCR0_AVX)) want |= XCR0_AVX;
            cpu::xsetbv(0, want & supported);
            s.xcr0 = cpu::xgetbv(0);
        }

        bool avx_on = (s.xcr0 & (XCR0_SSE | XCR0_AVX)) == (XCR0_SSE | XCR0_AVX);
        if (slot == 0) {
            g_xsave = xsave && s.xcr0 != 0;
            g_avx_state = avx_on;
            g_xcr0 = s.xcr0;
            u32 bytes = 512;
            if (g_xsave) {
                cpu::cpuid(0xD, 0, &a, &b, &c, &d);
                bytes = b;
            }
            g_state_bytes = bytes <= STATE_MAX_BYTES ? bytes : 0;
        }
        u64 flags = cpu::irq_save();
        s.probe = fpu_probe(avx_on ? 1 : 0);
        cpu::irq_restore(flags);
    }
    __atomic_or_fetch(&g_done_mask, 1ull << slot, __ATOMIC_SEQ_CST);
}

bool usable() { return ENABLED && g_state_bytes != 0; }
bool avx_state() { return g_avx_state; }
u32 state_bytes() { return g_state_bytes; }

void init_state(void* area) {
    auto* p = static_cast<u8*>(area);
    for (u32 i = 0; i < STATE_MAX_BYTES; ++i) p[i] = 0;
    if (!usable()) return;
    Modes m = default_modes();
    *reinterpret_cast<u16*>(p) = m.control_word;
    *reinterpret_cast<u32*>(p + 24) = m.mxcsr;
}

void save(void* area) {
    if (g_state_bytes == 0) return;
    if (g_xsave) {
        asm volatile("xsave64 (%0)" :: "r"(area), "a"(static_cast<u32>(g_xcr0)), "d"(static_cast<u32>(g_xcr0 >> 32)) : "memory");
    } else {
        asm volatile("fxsave64 (%0)" :: "r"(area) : "memory");
    }
}

void restore(const void* area) {
    if (g_state_bytes == 0) return;
    if (g_xsave) {
        asm volatile("xrstor64 (%0)" :: "r"(area), "a"(static_cast<u32>(g_xcr0)), "d"(static_cast<u32>(g_xcr0 >> 32)) : "memory");
    } else {
        asm volatile("fxrstor64 (%0)" :: "r"(area) : "memory");
    }
}

Modes default_modes() { return { 0x037F, 0x1F80 }; }

void set_modes(const Modes& m) { fpu_set_modes(m.control_word, m.mxcsr); }

Modes get_modes() {
    Modes m{};
    fpu_get_modes(&m.control_word, &m.mxcsr);
    return m;
}

u32 register_bytes() { return g_avx_state ? 16 * 32 : 16 * 16; }
void load_registers(const void* buf) { fpu_load_regs(buf, g_avx_state ? 1 : 0); }
void store_registers(void* buf) { fpu_store_regs(buf, g_avx_state ? 1 : 0); }

u32 cores_seen() {
    u64 m = __atomic_load_n(&g_done_mask, __ATOMIC_SEQ_CST);
    u32 n = 0;
    while (m) { n += static_cast<u32>(m & 1); m >>= 1; }
    return n;
}

bool selftest() {
    if (!ENABLED) return true;
    u64 mask = __atomic_load_n(&g_done_mask, __ATOMIC_SEQ_CST);
    if (!(mask & 1)) return false;
    const Seen& bsp = g_seen[0];
    for (u32 i = 0; i < MAX_CORES; ++i) {
        if (!(mask & (1ull << i))) continue;
        const Seen& s = g_seen[i];
        if (!s.sse2) return false;
        if (s.xcr0 != bsp.xcr0 || s.avx != bsp.avx || s.xsave != bsp.xsave) return false;
        u64 expect = PROBE_X87 | PROBE_SSE2;
        if ((s.xcr0 & (XCR0_SSE | XCR0_AVX)) == (XCR0_SSE | XCR0_AVX)) expect |= PROBE_AVX;
        if (s.probe != expect) return false;
    }
    return true;
}

void report() {
    u32 a, b, c, d;
    cpu::cpuid(1, 0, &a, &b, &c, &d);
    bool sse2 = (d & (1u << 26)) != 0, xsave = (c & (1u << 26)) != 0, avx = (c & (1u << 28)) != 0;
    cpu::cpuid(7, 0, &a, &b, &c, &d);
    bool avx2 = (b & (1u << 5)) != 0, avx512f = (b & (1u << 16)) != 0;
    serial::printf("[cpu] SIMD features: SSE2 %s, XSAVE %s, AVX %s, AVX2 %s, AVX-512F %s\n",
                    sse2 ? "yes" : "no", xsave ? "yes" : "no", avx ? "yes" : "no",
                    avx2 ? "yes" : "no", avx512f ? "yes" : "no");
    if (!ENABLED) {
        serial::writeln("[fpu] disabled: the kernel runs on general-purpose registers only (build with FPU=1 to enable)");
        return;
    }
    bool ok = selftest() && cores_seen() >= 1;
    serial::printf("[fpu] %u core(s) verified: x87 and SSE2 on, XCR0=%lx (%s)\n", cores_seen(), g_seen[0].xcr0,
                    (g_seen[0].xcr0 & XCR0_AVX) ? "AVX state enabled" : "no AVX state");
    serial::printf("[selftest] fpu: %s\n", ok ? "ok" : "MISMATCH");
}

}
