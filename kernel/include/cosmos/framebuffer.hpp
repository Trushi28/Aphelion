#pragma once
#include <cosmos/types.hpp>

namespace fb {

struct Info {
    volatile u8* base;
    u64 width, height, pitch;
    u8 bpp;
    u8 red_shift, green_shift, blue_shift;
};

bool init(const Info& info);
bool enable_backbuffer();
u32 enable_scrollback();
void clear(u32 rgb);
void put_pixel(u64 x, u64 y, u32 rgb);
void putc(char c, u32 fg, u32 bg);
void write(const char* s, u32 fg = 0xE0E0E0, u32 bg = 0x0A0A14);
void writeln(const char* s, u32 fg = 0xE0E0E0, u32 bg = 0x0A0A14);
void printf(u32 fg, const char* fmt, ...);
bool ready();
void enter_panic();

}
