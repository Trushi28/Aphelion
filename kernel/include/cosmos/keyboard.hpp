#pragma once
#include <cosmos/types.hpp>

namespace keyboard {

void init(u8 vector);

bool shift_down();
bool ctrl_down();
bool alt_down();

}
