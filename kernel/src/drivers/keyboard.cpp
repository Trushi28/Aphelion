#include <cosmos/keyboard.hpp>
#include <cosmos/input.hpp>
#include <cosmos/idt.hpp>
#include <cosmos/apic.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/serial.hpp>

namespace keyboard {

namespace {

constexpr u16 PORT_DATA = 0x60;
constexpr u16 PORT_STATUS_CMD = 0x64;

constexpr u8 ST_OUTPUT_FULL = 0x01;
constexpr u8 ST_INPUT_FULL = 0x02;
constexpr u8 ST_AUX = 0x20;
constexpr u8 ST_ERRORS = 0xC0;

constexpr u32 SPINS = 200000;
constexpr u32 FLUSH_LIMIT = 256;
constexpr u32 DRAIN_LIMIT = 32;

Status g_status = Status::NotStarted;
bool g_irq_routed = false;
u64 g_irq_count = 0;
u64 g_polled = 0;
u8 g_config = 0;
cpu::Spinlock g_lock;

bool wait_input_clear() {
    for (u32 i = 0; i < SPINS; ++i) {
        if (!(cpu::in8(PORT_STATUS_CMD) & ST_INPUT_FULL)) return true;
        cpu::io_wait();
    }
    return false;
}

bool wait_output_full() {
    for (u32 i = 0; i < SPINS; ++i) {
        if (cpu::in8(PORT_STATUS_CMD) & ST_OUTPUT_FULL) return true;
        cpu::io_wait();
    }
    return false;
}

bool controller_cmd(u8 cmd) {
    if (!wait_input_clear()) return false;
    cpu::out8(PORT_STATUS_CMD, cmd);
    return true;
}

bool controller_data(u8 data) {
    if (!wait_input_clear()) return false;
    cpu::out8(PORT_DATA, data);
    return true;
}

void flush_output() {
    for (u32 i = 0; i < FLUSH_LIMIT; ++i) {
        if (!(cpu::in8(PORT_STATUS_CMD) & ST_OUTPUT_FULL)) return;
        cpu::in8(PORT_DATA);
        cpu::io_wait();
    }
}

u32 drain_locked(bool from_poll) {
    u32 n = 0;
    while (n < DRAIN_LIMIT) {
        u8 st = cpu::in8(PORT_STATUS_CMD);
        if (st == 0xFF || !(st & ST_OUTPUT_FULL)) break;
        u8 b = cpu::in8(PORT_DATA);
        ++n;
        if (st & (ST_AUX | ST_ERRORS)) continue;
        if (from_poll) ++g_polled;
        input::feed_scancode(b);
    }
    return n;
}

void keyboard_isr(idt::Frame*) {
    g_lock.lock();
    ++g_irq_count;
    drain_locked(false);
    g_lock.unlock();
    apic::eoi();
}

}

Status init(u8 vector) {
    g_status = Status::NotStarted;

    u8 first = cpu::in8(PORT_STATUS_CMD);
    if (first == 0xFF) {
        g_status = Status::Absent;
        return g_status;
    }

    if (!controller_cmd(0xAD) || !controller_cmd(0xA7)) {
        g_status = Status::Unresponsive;
        return g_status;
    }
    flush_output();

    if (!controller_cmd(0x20) || !wait_output_full()) {
        g_status = Status::Unresponsive;
        return g_status;
    }
    u8 config = cpu::in8(PORT_DATA);
    config = static_cast<u8>((config | 0x01 | 0x40) & ~0x02 & ~0x10);
    if (!controller_cmd(0x60) || !controller_data(config)) {
        g_status = Status::Unresponsive;
        return g_status;
    }

    if (controller_cmd(0x20) && wait_output_full()) g_config = cpu::in8(PORT_DATA);
    else g_config = config;

    idt::set_handler(vector, &keyboard_isr);

    if (!controller_cmd(0xAE)) {
        g_status = Status::Unresponsive;
        return g_status;
    }

    if (controller_data(0xF4)) wait_output_full();
    g_lock.lock();
    drain_locked(false);
    g_lock.unlock();

    g_status = Status::Ready;
    return g_status;
}

Status status() { return g_status; }

const char* status_name(Status s) {
    switch (s) {
        case Status::NotStarted: return "not started";
        case Status::Absent: return "absent (no i8042 answers on ports 0x60/0x64)";
        case Status::Unresponsive: return "unresponsive (i8042 never became ready)";
        case Status::Ready: return "ready";
    }
    return "?";
}

void set_irq_routed(bool routed) { g_irq_routed = routed; }
bool irq_routed() { return g_irq_routed; }

void poll() {
    if (g_status != Status::Ready) return;
    u64 flags = cpu::irq_save();
    g_lock.lock();
    drain_locked(true);
    g_lock.unlock();
    cpu::irq_restore(flags);
}

u64 irq_count() { return g_irq_count; }
u64 polled_bytes() { return g_polled; }
u8 config_byte() { return g_config; }

}
