#pragma once
#include <cosmos/types.hpp>

namespace serial {

void init();
void putc(char c);
void write(const char* s);
void writeln(const char* s);

void printf(const char* fmt, ...);

}
