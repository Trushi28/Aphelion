#pragma once
#include <cosmos/types.hpp>
#include <cosmos/framebuffer.hpp>
#include <cosmos/term.hpp>

namespace corona {

constexpr u32 VOID = 0x0A0A14;
constexpr u32 TEXT = 0xD9DEE8;
constexpr u32 AMBER = 0xFFB347;
constexpr u32 EMBER = 0xFF7B3A;
constexpr u32 ICE = 0x8FD3FF;
constexpr u32 MINT = 0xC0FFC0;
constexpr u32 SAND = 0xE0D080;
constexpr u32 MIST = 0xB0B0C0;

struct Metrics {
    u32 cols;
    u32 rows;
    u32 cell_w;
    u32 cell_h;
    u64 fb_width;
    u64 fb_height;
    bool backbuffer;
};

const term::Palette& theme();

bool init(const fb::Info& info, const u8* font_blob, usize font_length);
bool ready();
bool enable_backbuffer(u32* memory, usize bytes);
usize backbuffer_bytes_needed();
usize scrollback_bytes_per_line();
u32 enable_scrollback(term::Cell* memory, usize bytes);
bool scroll_view(i32 lines);
bool snap_to_live();
u32 scrollback_lines();
u32 view_offset();
Metrics metrics();
term::Terminal& terminal();

void write(const char* s, u32 n);
void write(const char* s);
void write_colored(const char* s, u32 fg, u32 bg);
void put_pixel(u64 x, u64 y, u32 rgb);
void clear(u32 bg_rgb);
void flush();
void set_cursor_color(u32 rgb);

void enable_locking();
void enter_panic();
u32 pixel_at(u64 x, u64 y);
bool selftest();

}
