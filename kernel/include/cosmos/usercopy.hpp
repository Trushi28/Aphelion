#pragma once
#include <cosmos/types.hpp>

namespace usercopy {

struct Window {
    u8* host;
    u64 room;
};

constexpr bool range_ok(u64 uva, u64 n, u64 limit) { return n <= limit && uva <= limit - n; }

template <typename Reader>
bool copy_in(Reader& r, u64 uva, void* dst, u64 n) {
    if (!range_ok(uva, n, Reader::LIMIT)) return false;
    u8* out = static_cast<u8*>(dst);
    while (n) {
        Window w = r.window(uva);
        if (!w.host || !w.room) return false;
        u64 take = n < w.room ? n : w.room;
        __builtin_memcpy(out, w.host, take);
        out += take;
        uva += take;
        n -= take;
    }
    return true;
}

template <typename Writer>
bool copy_out(Writer& w, u64 uva, const void* src, u64 n) {
    if (!range_ok(uva, n, Writer::LIMIT)) return false;
    u64 probe = uva;
    u64 left = n;
    while (left) {
        Window win = w.window(probe);
        if (!win.host || !win.room) return false;
        u64 take = left < win.room ? left : win.room;
        probe += take;
        left -= take;
    }
    const u8* in = static_cast<const u8*>(src);
    while (n) {
        Window win = w.window(uva);
        u64 take = n < win.room ? n : win.room;
        __builtin_memcpy(win.host, in, take);
        in += take;
        uva += take;
        n -= take;
    }
    return true;
}

}
