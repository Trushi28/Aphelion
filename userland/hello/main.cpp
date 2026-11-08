#include <aphelion.hpp>

namespace {

u32 g_table[8] = { 3, 1, 4, 1, 5, 9, 2, 6 };
u64 g_zero[64];
const char g_text[] = "hello from a C++ APX program\n";
double g_ratio = 0.5;

struct Counter {
    u32 value;
    u32 step;
};

Counter g_counter = { 100, 7 };

u64 sum_table() {
    u64 total = 0;
    for (u32 v : g_table) total += v;
    return total;
}

bool bss_is_zero() {
    for (u64 v : g_zero) {
        if (v != 0) return false;
    }
    return true;
}

}

extern "C" i64 aphelion_main(u64 argument) {
    if (argument != 0) return 1;
    log(g_text);
    if (sum_table() != 31) return 2;
    if (!bss_is_zero()) return 3;
    g_zero[63] = 5;
    if (g_zero[63] != 5) return 4;
    g_counter.value += g_counter.step * 3;
    if (g_counter.value != 121) return 5;
    double x = g_ratio;
    for (int i = 0; i < 10; ++i) x = x * 1.5 + 0.25;
    if (x < 50.0 || x > 80.0) return 6;
    if (sys::gettid() < 0) return 7;
    sys::yield();
    return 42;
}
