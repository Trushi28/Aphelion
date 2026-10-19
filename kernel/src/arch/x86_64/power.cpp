#include <cosmos/power.hpp>
#include <cosmos/acpi.hpp>
#include <cosmos/cpu.hpp>
#include <cosmos/vmm.hpp>
#include <cosmos/serial.hpp>

namespace power {

namespace {

constexpr u16 KBC_STATUS_CMD = 0x64;
constexpr u16 PCI_RESET = 0xCF9;
constexpr u32 SPINS = 100000;

struct PACKED Idtr {
    u16 limit;
    u64 base;
};

void settle() {
    for (u32 i = 0; i < 2000; ++i) cpu::io_wait();
}

void acpi_reset() {
    const acpi::Info& a = acpi::info();
    if (!a.reset_supported) return;
    serial::printf("[power] reboot: ACPI reset register (space %u, address %lx, value %x)\n",
                   static_cast<unsigned>(a.reset_space), a.reset_address, static_cast<unsigned>(a.reset_value));
    if (a.reset_space == 1) {
        cpu::out8(static_cast<u16>(a.reset_address), a.reset_value);
    } else {
        auto* p = static_cast<volatile u8*>(constellation::map_firmware(a.reset_address, 1));
        if (p) *p = a.reset_value;
    }
    settle();
}

void keyboard_controller_reset() {
    serial::writeln("[power] reboot: keyboard controller pulse");
    for (u32 i = 0; i < SPINS; ++i) {
        if (!(cpu::in8(KBC_STATUS_CMD) & 0x02)) break;
        cpu::io_wait();
    }
    cpu::out8(KBC_STATUS_CMD, 0xFE);
    settle();
}

void pci_reset() {
    serial::writeln("[power] reboot: PCI reset control (0xCF9)");
    cpu::out8(PCI_RESET, 0x02);
    settle();
    cpu::out8(PCI_RESET, 0x06);
    settle();
}

[[noreturn]] void triple_fault() {
    serial::writeln("[power] reboot: triple fault");
    Idtr empty{0, 0};
    asm volatile("cli; lidt %0; int3" :: "m"(empty));
    cpu::hang();
}

}

bool in_hypervisor() {
    u32 a, b, c, d;
    cpu::cpuid(1, 0, &a, &b, &c, &d);
    return (c >> 31) & 1;
}

Capabilities capabilities() {
    Capabilities caps{};
    caps.hypervisor = in_hypervisor();
    caps.acpi_reset = acpi::info().reset_supported;
    caps.poweroff_supported = caps.hypervisor;
    caps.poweroff_path = caps.hypervisor ? "hypervisor power-off port" : "none yet (needs the ACPI sleep-state tables)";
    return caps;
}

NORETURN void reboot() {
    cpu::cli();
    acpi_reset();
    keyboard_controller_reset();
    pci_reset();
    triple_fault();
}

bool poweroff() {
    if (!in_hypervisor()) {
        serial::writeln("[power] poweroff: not available on real hardware yet");
        return false;
    }
    serial::writeln("[power] poweroff: hypervisor ports");
    cpu::out16(0x604, 0x2000);
    cpu::out16(0xB004, 0x2000);
    cpu::out16(0x4004, 0x3400);
    settle();
    serial::writeln("[power] poweroff: the hypervisor ignored every port");
    return false;
}

NORETURN void halt_forever() {
    cpu::hang();
}

}
