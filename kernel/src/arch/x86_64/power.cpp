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
constexpr u16 PM1_SCI_EN = 1u << 0;
constexpr u16 PM1_SLP_TYP_MASK = 7u << 10;
constexpr u16 PM1_SLP_EN = 1u << 13;

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

void write_sleep(u16 port, u8 typ) {
    u16 v = cpu::in16(port);
    u16 base = static_cast<u16>((v & ~PM1_SLP_TYP_MASK & ~PM1_SLP_EN) | (static_cast<u16>(typ) << 10));
    cpu::out16(port, base);
    cpu::out16(port, static_cast<u16>(base | PM1_SLP_EN));
}

bool acpi_poweroff() {
    const acpi::Info& a = acpi::info();
    if (!a.s5_found || a.pm1a_cnt == 0) return false;
    serial::printf("[power] poweroff: ACPI S5 (PM1a %x, SLP_TYP %u)\n", static_cast<unsigned>(a.pm1a_cnt),
                   static_cast<unsigned>(a.slp_typ_a));
    if (!(cpu::in16(a.pm1a_cnt) & PM1_SCI_EN) && a.smi_cmd != 0 && a.acpi_enable != 0) {
        serial::writeln("[power] poweroff: enabling ACPI mode through the SMI command port");
        cpu::out8(a.smi_cmd, a.acpi_enable);
        for (u32 i = 0; i < SPINS; ++i) {
            if (cpu::in16(a.pm1a_cnt) & PM1_SCI_EN) break;
            cpu::io_wait();
        }
    }
    write_sleep(a.pm1a_cnt, a.slp_typ_a);
    if (a.pm1b_cnt != 0) write_sleep(a.pm1b_cnt, a.slp_typ_b);
    settle();
    return true;
}

void hypervisor_poweroff() {
    serial::writeln("[power] poweroff: hypervisor ports");
    cpu::out16(0x604, 0x2000);
    cpu::out16(0xB004, 0x2000);
    cpu::out16(0x4004, 0x3400);
    settle();
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
    const acpi::Info& a = acpi::info();
    const bool acpi_s5 = a.s5_found && a.pm1a_cnt != 0;
    caps.poweroff_supported = acpi_s5 || caps.hypervisor;
    caps.poweroff_path = acpi_s5 ? "ACPI S5 sleep state"
                                 : (caps.hypervisor ? "hypervisor power-off port" : "none (no _S5_ object in the ACPI tables)");
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
    const u64 flags = cpu::irq_save();
    acpi_poweroff();
    if (in_hypervisor()) hypervisor_poweroff();
    serial::writeln("[power] poweroff: the platform did not power off");
    cpu::irq_restore(flags);
    return false;
}

NORETURN void halt_forever() {
    cpu::hang();
}

}
