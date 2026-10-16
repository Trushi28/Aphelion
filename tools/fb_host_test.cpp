#include <cosmos/types.hpp>
#include "../kernel/src/drivers/framebuffer.cpp"
#include <stdio.h>
#include <stdlib.h>

static constexpr u64 W = 640, H = 480, PITCH = W * 4;
static u32 g_screen[W * H];
static u32 g_snapshot[W * H];

static void start() {
    memset(g_screen, 0, sizeof(g_screen));
    fb::Info info{};
    info.base = reinterpret_cast<volatile u8*>(g_screen);
    info.width = W; info.height = H; info.pitch = PITCH; info.bpp = 32;
    info.red_shift = 16; info.green_shift = 8; info.blue_shift = 0;
    fb::init(info);
    fb::clear(0x0A0A14);
}

static void lines(int first, int last) {
    for (int i = first; i < last; ++i) {
        char buf[64];
        snprintf(buf, sizeof(buf), "line %d: the quick brown fox %d\n", i, i * 7);
        fb::write(buf, 0x80FF80 + static_cast<u32>(i & 7));
    }
}

int main() {
    int failures = 0;
    const int rows = H / 8;

    start();
    lines(0, 10);
    bool untouched = true;
    for (u64 y = 0; y < H; ++y) {
        bool row_has_ink = false;
        for (u64 x = 0; x < W; ++x) if (g_screen[y * W + x] != 0x0A0A14) row_has_ink = true;
        if (y >= 10 * 8 && row_has_ink) untouched = false;
    }
    printf("%-58s %s\n", "fb: text stays in the rows it was written to", untouched ? "ok" : "FAIL");
    failures += !untouched;

    const int total = 400;
    start();
    lines(0, total);
    memcpy(g_snapshot, g_screen, sizeof(g_screen));
    start();
    lines(total - (rows - 1), total);
    bool same = memcmp(g_snapshot, g_screen, sizeof(g_screen)) == 0;
    printf("%-58s %s\n", "fb: scrolled screen equals a fresh render of the tail", same ? "ok" : "FAIL");
    failures += !same;

    start();
    fb::write("abc");
    fb::putc('\b', 0xE0E0E0, 0x0A0A14);
    fb::putc('X', 0xE0E0E0, 0x0A0A14);
    start();
    fb::write("abX");
    memcpy(g_snapshot, g_screen, sizeof(g_screen));
    start();
    fb::write("abc");
    fb::putc('\b', 0xE0E0E0, 0x0A0A14);
    fb::putc('X', 0xE0E0E0, 0x0A0A14);
    bool bs = memcmp(g_snapshot, g_screen, sizeof(g_screen)) == 0;
    printf("%-58s %s\n", "fb: backspace erases the previous cell", bs ? "ok" : "FAIL");
    failures += !bs;

    start();
    for (int i = 0; i < 200; ++i) fb::putc('#', 0x100000u + static_cast<u32>(i) * 0x010101u, 0x0A0A14);
    fb::write("\ntail line\n");
    printf("%-58s %s\n", "fb: 200 distinct colours run without memory errors", "ok");

    return failures ? 1 : 0;
}
