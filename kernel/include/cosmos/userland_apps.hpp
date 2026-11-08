#pragma once
#include <cosmos/types.hpp>

namespace userland::apps {

enum class Outcome : u32 {
    Finished,
    Unavailable,
    BadName,
    NotFound,
    LaunchFailed,
    Interrupted,
};

struct Report {
    Outcome outcome;
    i32 status;
    i64 error;
};

using Emit = void (*)(void* ctx, const char* text, u32 length);
using Interrupted = bool (*)(void* ctx);

bool install();
Report run(const char* name, Emit emit, Interrupted interrupted, void* ctx);

}
