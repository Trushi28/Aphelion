#include "internal.hpp"
#include <cosmos/handleabi.hpp>
#include <cosmos/lens_channel.hpp>

namespace userland::impl {

i64 sys_handle_close(idt::Frame* f) {
    Process* p = current_process();
    TableGuard guard(p->handles.lock);
    return handleabi::error_of(lens::close(p->handles.domain, lens::Lens(f->rdi)).status());
}

i64 sys_handle_info(idt::Frame* f) {
    Process* p = current_process();
    TableGuard guard(p->handles.lock);
    lens::Lens l(f->rdi);
    lens::Result<lens::Rights> r = lens::rights_of(p->handles.domain, l);
    if (!r.ok()) return handleabi::error_of(r.status());
    lens::Result<lens::NodeKind> k = lens::kind_of(p->handles.domain, l);
    if (!k.ok()) return handleabi::error_of(k.status());
    u64 subtype = handleabi::SUBTYPE_NONE;
    if (k.value() == lens::NodeKind::Stream) {
        lens::Result<lens::Slot*> sr = lens::slot_of(p->handles.domain, l);
        if (sr.ok() && sr.value()->stream) {
            u32 type = sr.value()->stream->type();
            if (type == lens::channel::STREAM_TYPE) subtype = handleabi::SUBTYPE_CHANNEL;
            else if (type == memobj::STREAM_TYPE) subtype = handleabi::SUBTYPE_MEMOBJ;
            else if (type == PROCESS_STREAM_TYPE) subtype = handleabi::SUBTYPE_PROCESS;
        }
    }
    return static_cast<i64>(handleabi::info_pack(r.value().bits, static_cast<u8>(k.value()), static_cast<u8>(subtype)));
}

i64 sys_handle_narrow(idt::Frame* f) {
    Process* p = current_process();
    u64 zero = 0;
    if (!user_write(p, f->rdx, &zero, sizeof(zero))) return userabi::ERR_FAULT;
    TableGuard guard(p->handles.lock);
    lens::Result<lens::Lens> r = lens::narrow(p->handles.domain, lens::Lens(f->rdi), lens::Rights::from_bits(static_cast<u32>(f->rsi)));
    if (!r.ok()) return handleabi::error_of(r.status());
    u64 raw = r.value().raw;
    user_write(p, f->rdx, &raw, sizeof(raw));
    return 0;
}

i64 sys_channel_create(idt::Frame* f) {
    Process* p = current_process();
    u64 pair[2] = { 0, 0 };
    if (!user_write(p, f->rdi, pair, sizeof(pair))) return userabi::ERR_FAULT;
    lens::Lens a, b;
    lens::Done d = lens::channel::create(p->handles, &a, &b);
    if (!d.ok()) return handleabi::error_of(d.status());
    pair[0] = a.raw;
    pair[1] = b.raw;
    user_write(p, f->rdi, pair, sizeof(pair));
    return 0;
}

i64 sys_channel_send(idt::Frame* f) {
    Process* p = current_process();
    u64 len = f->rdx;
    u64 count = f->r8;
    if (len > lens::channel::MAX_BYTES || count > lens::channel::MAX_HANDLES) return handleabi::ERR_TOOBIG;
    u8 bytes[lens::channel::MAX_BYTES];
    u64 raw[lens::channel::MAX_HANDLES];
    if (len && !user_read(p, f->rsi, bytes, len)) return userabi::ERR_FAULT;
    if (count && !user_read(p, f->r10, raw, count * sizeof(u64))) return userabi::ERR_FAULT;
    lens::Lens handles[lens::channel::MAX_HANDLES];
    for (u64 i = 0; i < count; ++i) handles[i] = lens::Lens(raw[i]);
    lens::Done d = lens::channel::send(p->handles, lens::Lens(f->rdi), lens::ConstBytes(bytes, len), handles, static_cast<u32>(count));
    return handleabi::error_of(d.status());
}

i64 sys_channel_recv(idt::Frame* f) {
    Process* p = current_process();
    u64 cap = f->rdx < lens::channel::MAX_BYTES ? f->rdx : lens::channel::MAX_BYTES;
    u64 hcap = f->r8 < lens::channel::MAX_HANDLES ? f->r8 : lens::channel::MAX_HANDLES;
    u8 bytes[lens::channel::MAX_BYTES] = {};
    u64 raw[lens::channel::MAX_HANDLES] = {};
    if (cap && !user_write(p, f->rsi, bytes, cap)) return userabi::ERR_FAULT;
    if (hcap && !user_write(p, f->r10, raw, hcap * sizeof(u64))) return userabi::ERR_FAULT;
    lens::Lens handles[lens::channel::MAX_HANDLES];
    bool block = (f->r9 & handleabi::RECV_BLOCK) != 0;
    lens::Result<lens::channel::Received> r = lens::channel::receive(p->handles, lens::Lens(f->rdi), lens::Bytes(bytes, cap), handles,
                                                                     static_cast<u32>(hcap), block, &group_dying, p);
    if (!r.ok()) return handleabi::error_of(r.status());
    lens::channel::Received got = r.value();
    for (u32 i = 0; i < got.handles; ++i) raw[i] = handles[i].raw;
    bool wrote = (got.bytes == 0 || user_write(p, f->rsi, bytes, got.bytes)) &&
                 (got.handles == 0 || user_write(p, f->r10, raw, got.handles * sizeof(u64)));
    if (!wrote) {
        TableGuard guard(p->handles.lock);
        for (u32 i = 0; i < got.handles; ++i) (void)lens::close(p->handles.domain, handles[i]);
        return userabi::ERR_FAULT;
    }
    return handleabi::recv_pack(got.bytes, got.handles);
}

i64 sys_handle_wait(idt::Frame* f) {
    Process* p = current_process();
    u64 count = f->rsi;
    if (count == 0 || count > handleabi::MAX_WAIT_ENTRIES) return userabi::ERR_INVAL;
    handleabi::WaitEntry entries[handleabi::MAX_WAIT_ENTRIES];
    if (!user_read(p, f->rdi, entries, count * sizeof(handleabi::WaitEntry))) return userabi::ERR_FAULT;
    lens::Wait waits[handleabi::MAX_WAIT_ENTRIES];
    for (u64 i = 0; i < count; ++i) {
        if (!handleabi::wait_want_ok(entries[i].want)) return userabi::ERR_INVAL;
        lens::Readiness want;
        want.bits = static_cast<u8>(entries[i].want);
        waits[i] = lens::Wait{ lens::Lens(entries[i].handle), want, lens::Readiness() };
    }
    if (!user_write(p, f->rdi, entries, count * sizeof(handleabi::WaitEntry))) return userabi::ERR_FAULT;
    lens::Stream* streams[handleabi::MAX_WAIT_ENTRIES];
    {
        TableGuard guard(p->handles.lock);
        lens::Result<u32> pinned = lens::acquire(p->handles.domain, waits, static_cast<u32>(count), streams);
        if (!pinned.ok()) return handleabi::error_of(pinned.status());
    }
    lens::Result<u32> r = lens::await_streams(streams, waits, static_cast<u32>(count), f->rdx, &group_dying, p);
    lens::unpin(streams, static_cast<u32>(count));
    if (!r.ok()) return handleabi::error_of(r.status());
    for (u64 i = 0; i < count; ++i) entries[i].got = waits[i].got.bits;
    user_write(p, f->rdi, entries, count * sizeof(handleabi::WaitEntry));
    return static_cast<i64>(r.value());
}

}
