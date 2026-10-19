#pragma once
#include <cosmos/types.hpp>

namespace keyboard {

enum class Status : u8 { NotStarted, Absent, Unresponsive, Ready };

Status init(u8 vector);
Status status();
const char* status_name(Status s);

void set_irq_routed(bool routed);
bool irq_routed();

void poll();

u64 irq_count();
u64 polled_bytes();
u8 config_byte();

}
