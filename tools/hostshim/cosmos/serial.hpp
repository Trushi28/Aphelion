#pragma once
#include <stdio.h>
#include <stdarg.h>

namespace serial {

inline void printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}
inline void writeln(const char* s) { puts(s); }

}
