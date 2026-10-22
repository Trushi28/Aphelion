#pragma once
#include <cosmos/types.hpp>

namespace orbital {

class Mutex {
public:
    void lock() { ++locks; }
    void unlock() { ++unlocks; }
    int locks = 0;
    int unlocks = 0;
};

}
