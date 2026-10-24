#pragma once
#include <cosmos/types.hpp>
#include <cosmos/keymap.hpp>

namespace input {

void init();
bool ready();
void set_wake_hook(void (*hook)());

bool push(const Key& key);
void feed_scancode(u8 byte);
void feed_serial(u8 byte);

bool try_read(Key& out);
void read(Key& out);
bool read_for(Key& out, u64 timeout_ms);

u32 pending();
u64 dropped();
u64 total();

void pump();
void flush();

bool selftest();

}
