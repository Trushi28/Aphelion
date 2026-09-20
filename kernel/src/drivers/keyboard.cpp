#include <cosmos/keyboard.hpp>
#include <cosmos/idt.hpp>
#include <cosmos/apic.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/framebuffer.hpp>

namespace keyboard {

constexpr u16 PORT_DATA = 0x60;
constexpr u16 PORT_STATUS_CMD = 0x64;

static void wait_input_clear() { while (cpu::in8(PORT_STATUS_CMD) & 0x02) cpu::io_wait(); }
static void wait_output_full() { while (!(cpu::in8(PORT_STATUS_CMD) & 0x01)) cpu::io_wait(); }

static void controller_cmd(u8 cmd) { wait_input_clear(); cpu::out8(PORT_STATUS_CMD, cmd); }
static void controller_write_data(u8 data) { wait_input_clear(); cpu::out8(PORT_DATA, data); }

static char g_set1_to_ascii[128];

static void build_scancode_table() {
    for (auto& c : g_set1_to_ascii) c = 0;
    const char* row1 = "1234567890-=";
    for (int i = 0; row1[i]; ++i) g_set1_to_ascii[0x02 + i] = row1[i];
    g_set1_to_ascii[0x0E] = '\b';
    g_set1_to_ascii[0x0F] = '\t';
    const char* row2 = "qwertyuiop[]";
    for (int i = 0; row2[i]; ++i) g_set1_to_ascii[0x10 + i] = row2[i];
    g_set1_to_ascii[0x1C] = '\n';
    const char* row3 = "asdfghjkl;'`";
    for (int i = 0; row3[i]; ++i) g_set1_to_ascii[0x1E + i] = row3[i];
    g_set1_to_ascii[0x2B] = '\\';
    const char* row4 = "zxcvbnm,./";
    for (int i = 0; row4[i]; ++i) g_set1_to_ascii[0x2C + i] = row4[i];
    g_set1_to_ascii[0x39] = ' ';
}

static void keyboard_isr(idt::Frame*) {
    apic::eoi();
    u8 sc = cpu::in8(PORT_DATA);
    if (sc & 0x80) return;
    char c = g_set1_to_ascii[sc & 0x7F];
    if (!c) return;
    serial::putc(c);
    if (fb::ready()) fb::putc(c, 0xE8E8E8, 0x0A0A14);
}

void init(u8 vector) {
    build_scancode_table();

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
