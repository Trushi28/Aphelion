#pragma once
#include <cosmos/types.hpp>
#include <cosmos/lens.hpp>

namespace userland::apps {

enum class Outcome : u32 {
    Finished,
    Unavailable,
    BadName,
    NotFound,
    LaunchFailed,
    Interrupted,
    BadArguments,
    InputTooLarge,
};

struct Report {
    Outcome outcome;
    i32 status;
    i64 error;
};

using Emit = void (*)(void* ctx, const char* text, u32 length);
using Interrupted = bool (*)(void* ctx);

constexpr u32 MAX_INPUT = 65536;

struct Request {
    const char* name = nullptr;
    const char* const* args = nullptr;
    u32 arg_count = 0;
    const char* const* env = nullptr;
    u32 env_count = 0;
    const char* input = nullptr;
    u32 input_length = 0;
    lens::Domain* work_domain = nullptr;
    lens::Lens work;
    Emit emit = nullptr;
    Interrupted interrupted = nullptr;
    void* ctx = nullptr;
};

bool install();
bool exists(const char* name);
Report run(const Request& request);
Report run(const char* name, Emit emit, Interrupted interrupted, void* ctx);

}
