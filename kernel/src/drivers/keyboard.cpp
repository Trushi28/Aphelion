#include <cosmos/keyboard.hpp>
#include <cosmos/idt.hpp>
#include <cosmos/apic.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/framebuffer.hpp>

namespace keyboard {

constexpr u16 PORT_DATA = 0x60;
constexpr u16 PORT_STATUS_CMD = 0x64;

constexpr u8 SC_LSHIFT = 0x2A;
constexpr u8 SC_RSHIFT = 0x36;
constexpr u8 SC_LCTRL = 0x1D;
constexpr u8 SC_LALT = 0x38;

static void wait_input_clear() { while (cpu::in8(PORT_STATUS_CMD) & 0x02) cpu::io_wait(); }
static void wait_output_full() { while (!(cpu::in8(PORT_STATUS_CMD) & 0x01)) cpu::io_wait(); }

static void controller_cmd(u8 cmd) { wait_input_clear(); cpu::out8(PORT_STATUS_CMD, cmd); }
static void controller_write_data(u8 data) { wait_input_clear(); cpu::out8(PORT_DATA, data); }

static char g_set1_to_ascii[128];
static char g_set1_to_ascii_shifted[128];

static volatile bool g_shift = false;
static volatile bool g_ctrl = false;
static volatile bool g_alt = false;

bool shift_down() { return g_shift; }
bool ctrl_down() { return g_ctrl; }
bool alt_down() { return g_alt; }

static void fill_row(char* table, u8 start, const char* chars) {
    for (int i = 0; chars[i]; ++i) table[start + i] = chars[i];
}

static void build_scancode_tables() {
    for (auto& c : g_set1_to_ascii) c = 0;
    for (auto& c : g_set1_to_ascii_shifted) c = 0;

    fill_row(g_set1_to_ascii, 0x02, "1234567890-=");
    fill_row(g_set1_to_ascii_shifted, 0x02, "!@#$%^&*()_+");
    g_set1_to_ascii[0x0E] = g_set1_to_ascii_shifted[0x0E] = '\b';
    g_set1_to_ascii[0x0F] = g_set1_to_ascii_shifted[0x0F] = '\t';

    fill_row(g_set1_to_ascii, 0x10, "qwertyuiop[]");
    fill_row(g_set1_to_ascii_shifted, 0x10, "QWERTYUIOP{}");
    g_set1_to_ascii[0x1C] = g_set1_to_ascii_shifted[0x1C] = '\n';

    fill_row(g_set1_to_ascii, 0x1E, "asdfghjkl;'`");
    fill_row(g_set1_to_ascii_shifted, 0x1E, "ASDFGHJKL:\"~");

    g_set1_to_ascii[0x2B] = '\\';
    g_set1_to_ascii_shifted[0x2B] = '|';

    fill_row(g_set1_to_ascii, 0x2C, "zxcvbnm,./");
    fill_row(g_set1_to_ascii_shifted, 0x2C, "ZXCVBNM<>?");

    g_set1_to_ascii[0x39] = g_set1_to_ascii_shifted[0x39] = ' ';
}

static void keyboard_isr(idt::Frame*) {
    apic::eoi();
    u8 sc = cpu::in8(PORT_DATA);
    bool release = sc & 0x80;
    u8 code = sc & 0x7F;

    if (code == SC_LSHIFT || code == SC_RSHIFT) { g_shift = !release; return; }
    if (code == SC_LCTRL) { g_ctrl = !release; return; }
    if (code == SC_LALT) { g_alt = !release; return; }

    if (release) return;

    char c = (g_shift ? g_set1_to_ascii_shifted : g_set1_to_ascii)[code];
    if (!c) return;
    serial::putc(c);
    if (fb::ready()) fb::putc(c, 0xE8E8E8, 0x0A0A14);
}

void init(u8 vector) {
    build_scancode_tables();

    controller_cmd(0xAD);
    controller_cmd(0xA7);

    while (cpu::in8(PORT_STATUS_CMD) & 0x01) cpu::in8(PORT_DATA);

    controller_cmd(0x20);
    wait_output_full();
    u8 config = cpu::in8(PORT_DATA);
    config |= 0x01;
    config &= ~0x10;
    controller_cmd(0x60);
    controller_write_data(config);

    controller_cmd(0xAE);

    idt::set_handler(vector, &keyboard_isr);
}

}
