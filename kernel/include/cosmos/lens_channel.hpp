#pragma once
#include <cosmos/lens.hpp>
#include <cosmos/orbital.hpp>

namespace lens {

struct Table {
    Domain domain;
    orbital::Mutex lock;
};

namespace channel {

constexpr u32 MAX_BYTES = 256;
constexpr u32 MAX_HANDLES = 4;
constexpr u32 QUEUE_DEPTH = 16;
constexpr u32 STREAM_TYPE = 0x4C4E4843;

constexpr Rights END_RIGHTS = Right::Read | Right::Write | Right::Delegate;

struct Received {
    u32 bytes;
    u32 handles;
};

Done create(Table& t, Lens* a, Lens* b);
Done send(Table& t, Lens ch, ConstBytes data, const Lens* handles, u32 count);
Result<Received> receive(Table& t, Lens ch, Bytes data, Lens* handles, u32 handle_cap, bool block, CancelFn cancel, void* cancel_ctx);

}

}
