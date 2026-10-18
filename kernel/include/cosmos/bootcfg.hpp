#pragma once
#include <cosmos/types.hpp>

namespace bootcfg {

enum class Mode : u8 { Test, Shell };
enum class Storage : u8 { Auto, Off };
enum class TimerPref : u8 { Auto, Hpet, Pm, Pit };

struct Config {
    Mode mode = Mode::Test;
    Storage storage = Storage::Auto;
    TimerPref timer = TimerPref::Auto;
    u32 rejected = 0;
};

Config parse(const char* cmdline);

void set(const Config& c);
const Config& get();

const char* name(Mode m);
const char* name(Storage s);
const char* name(TimerPref t);

}
