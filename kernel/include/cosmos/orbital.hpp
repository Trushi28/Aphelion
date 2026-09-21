#pragma once
#include <cosmos/types.hpp>
#include <cosmos/idt.hpp>

namespace orbital {

constexpr int NUM_RINGS = 4;

using EntryFn = void (*)(void* arg);

struct Satellite {
    u64 rsp = 0;
    u8* stack_base = nullptr;
    u64 stack_size = 0;
    int ring = 0;
    u32 ticks_left = 0;
    bool in_use = false;
    EntryFn entry = nullptr;
    void* arg = nullptr;
    const char* name = "?";
    Satellite* next = nullptr;
};

void init(u64 hhdm_offset);

void init_core();

Satellite* spawn(const char* name, EntryFn entry, void* arg);

void yield();
NORETURN void exit_current();

NORETURN void start_core();

}
