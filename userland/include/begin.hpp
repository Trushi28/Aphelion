#pragma once
#include "aphelion.hpp"
#include <cosmos/handleabi.hpp>
#include <cosmos/startup.hpp>

namespace begin {

struct Start {
    startup::Parsed parsed;
    u64 handle[startup::SLOT_COUNT];
    u8 raw[startup::MAX_MESSAGE];
};

inline bool receive(u64 channel, Start* out) {
    for (u32 i = 0; i < startup::SLOT_COUNT; ++i) out->handle[i] = 0;
    u64 received[startup::MAX_HANDLES] = {};
    i64 got = sys::channel_recv(channel, out->raw, sizeof(out->raw), received, startup::MAX_HANDLES, handleabi::RECV_BLOCK);
    if (got < 0) return false;
    u32 length = handleabi::recv_bytes(got);
    u32 count = handleabi::recv_handles(got);
    bool ok = startup::decode(out->raw, length, count, &out->parsed) == startup::OK;
    u32 next = 0;
    for (u32 slot = 0; slot < startup::SLOT_COUNT; ++slot) {
        if (ok && startup::has(out->parsed.present, slot)) out->handle[slot] = received[next++];
    }
    if (!ok) {
        for (u32 i = 0; i < count; ++i) sys::handle_close(received[i]);
        return false;
    }
    sys::handle_close(channel);
    return true;
}

}
