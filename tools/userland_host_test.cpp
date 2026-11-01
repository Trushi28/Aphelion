#include <cosmos/types.hpp>
#include <cosmos/userabi.hpp>
#include <cosmos/usercopy.hpp>
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
    static_assert(userabi::SYS_COUNT == 4, "the native table has four entries in this series");
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

int main() {
    range_tests();
    copy_tests();
    abi_tests();
    printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
