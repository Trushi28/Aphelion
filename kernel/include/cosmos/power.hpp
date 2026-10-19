#pragma once
#include <cosmos/types.hpp>

namespace power {

enum class ResetPath : u8 { AcpiRegister, KeyboardController, PciReset, TripleFault };

struct Capabilities {
    bool hypervisor;
    bool acpi_reset;
    bool poweroff_supported;
    const char* poweroff_path;
};

Capabilities capabilities();

NORETURN void reboot();
bool poweroff();
NORETURN void halt_forever();

bool in_hypervisor();

}
