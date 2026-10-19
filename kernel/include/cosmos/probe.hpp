#pragma once
#include <cosmos/types.hpp>

namespace probe {

struct Facts {
    u32 suns;
    const char* storage;
    bool keyboard_ready;
    bool keyboard_irq;
    bool serial_present;
    bool serial_irq;
};

void set_facts(const Facts& f);
bool spawn();

}
