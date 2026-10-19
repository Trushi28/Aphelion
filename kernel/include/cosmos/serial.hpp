#pragma once
#include <cosmos/types.hpp>

namespace serial {

void init();
bool present();
bool enable_rx_irq(u8 vector);
bool rx_irq_enabled();
void poll_rx();
u64 rx_bytes();
u64 tx_drops();
void putc(char c);
void console_write(const char* s, u32 n);
void write(const char* s);
void writeln(const char* s);

void printf(const char* fmt, ...);

}
