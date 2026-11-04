#include <cosmos/lens.hpp>
#include <cosmos/orbital.hpp>
#include <cosmos/clock.hpp>
#include <cosmos/kheap.hpp>

namespace lens {

namespace {

constexpr usize MIN_PIPE = 64;
constexpr usize MAX_PIPE = 1u << 20;

orbital::WaitQueue g_events;

struct Pipe;

class PipeEnd final : public Stream {
public:
    Pipe* pipe;
    bool reader;
    u32 refs;

    const char* name() const override { return reader ? "pipe:read" : "pipe:write"; }
    Result<usize> read(Bytes dst) override;
    Result<usize> write(ConstBytes src) override;
    Readiness ready() override;
    void retain() override { __atomic_add_fetch(&refs, 1, __ATOMIC_ACQ_REL); }
    void release() override;
};

struct Pipe {
    u8* buf;
    usize mask;
    u64 rd;
    u64 wr;
    orbital::Mutex rd_lock;
    orbital::Mutex wr_lock;
    u32 open_ends;
    bool reader_gone;
    bool writer_gone;
    PipeEnd read_end;
    PipeEnd write_end;
};

u64 load(const u64* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
bool flag(const bool* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }

struct Hold {
    orbital::Mutex& m;
    explicit Hold(orbital::Mutex& x) : m(x) { m.lock(); }
    ~Hold() { m.unlock(); }
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;
};

Result<usize> PipeEnd::read(Bytes dst) {
    if (!reader) return Result<usize>::fail(Status::Denied);
    Pipe& p = *pipe;
    usize n = 0;
    {
        Hold hold(p.rd_lock);
        u64 rd = load(&p.rd);
        u64 avail = load(&p.wr) - rd;
        if (avail == 0) {
            if (flag(&p.writer_gone)) {
                avail = load(&p.wr) - rd;
                if (avail == 0) return Result<usize>::success(0);
            } else {
                return Result<usize>::fail(Status::NotReady);
            }
        }
        n = dst.len < avail ? dst.len : static_cast<usize>(avail);
        for (usize i = 0; i < n; ++i) dst.ptr[i] = p.buf[(rd + i) & p.mask];
        __atomic_store_n(&p.rd, rd + n, __ATOMIC_RELEASE);
    }
    if (n) stream_event();
    return Result<usize>::success(n);
}

Result<usize> PipeEnd::write(ConstBytes src) {
    if (reader) return Result<usize>::fail(Status::Denied);
    Pipe& p = *pipe;
    if (flag(&p.reader_gone)) return Result<usize>::fail(Status::Closed);
    if (src.len == 0) return Result<usize>::success(0);
    usize n = 0;
    {
        Hold hold(p.wr_lock);
        u64 wr = load(&p.wr);
        u64 room = (p.mask + 1) - (wr - load(&p.rd));
        if (room == 0) return Result<usize>::fail(Status::NotReady);
        n = src.len < room ? src.len : static_cast<usize>(room);
        for (usize i = 0; i < n; ++i) p.buf[(wr + i) & p.mask] = src.ptr[i];
        __atomic_store_n(&p.wr, wr + n, __ATOMIC_RELEASE);
    }
    stream_event();
    return Result<usize>::success(n);
}

Readiness PipeEnd::ready() {
    Pipe& p = *pipe;
    Readiness r;
    if (reader) {
        if (load(&p.wr) != load(&p.rd) || flag(&p.writer_gone)) r = r | Ready::Readable;
        if (flag(&p.writer_gone)) r = r | Ready::Hangup;
    } else {
        if (flag(&p.reader_gone)) r = r | Ready::Hangup;
        else if ((p.mask + 1) - (load(&p.wr) - load(&p.rd)) > 0) r = r | Ready::Writable;
    }
    return r;
}

void PipeEnd::release() {
    if (__atomic_sub_fetch(&refs, 1, __ATOMIC_ACQ_REL) != 0) return;
    Pipe* p = pipe;
    if (reader) __atomic_store_n(&p->reader_gone, true, __ATOMIC_RELEASE);
    else __atomic_store_n(&p->writer_gone, true, __ATOMIC_RELEASE);
    stream_event();
    if (__atomic_sub_fetch(&p->open_ends, 1, __ATOMIC_ACQ_REL) == 0) {
        kheap::free(p->buf);
        kheap::free(p);
    }
}

struct AwaitCtx {
    Wait* waits;
    Stream** streams;
    u32 count;
    CancelFn cancel;
    void* cancel_ctx;
};

Readiness filtered(Stream* s, Readiness want) { return s->ready() & (want | Ready::Hangup); }

bool any_ready(void* p) {
    AwaitCtx* c = static_cast<AwaitCtx*>(p);
    if (c->cancel && c->cancel(c->cancel_ctx)) return true;
    for (u32 i = 0; i < c->count; ++i)
        if (filtered(c->streams[i], c->waits[i].want).any()) return true;
    return false;
}

u64 deadline_after(u64 now, u64 timeout_ms) {
    return timeout_ms > ~0ull - now ? ~0ull : now + timeout_ms;
}

}

void stream_event() { g_events.wake_all(); }

Result<Lens> mint_stream(Domain& d, Stream* stream, Rights rights) {
    if (sealed()) return Result<Lens>::fail(Status::Denied);
    if (!stream) return Result<Lens>::fail(Status::InvalidPath);
    return alloc_slot(d, nullptr, nebula::Node{0, 0}, nebula::Node{0, 0}, rights, false, 0, stream->name(), stream);
}

Done make_pipe(Domain& d, usize capacity, Lens* read_end, Lens* write_end) {
    *read_end = Lens();
    *write_end = Lens();
    if (d.live + 2 > MAX_LENSES) return Done::fail(Status::TableFull);
    usize cap = MIN_PIPE;
    while (cap < capacity && cap < MAX_PIPE) cap <<= 1;

    Pipe* p = static_cast<Pipe*>(kheap::alloc_zeroed(sizeof(Pipe)));
    if (!p) return Done::fail(Status::NoMemory);
    p->buf = static_cast<u8*>(kheap::alloc(cap));
    if (!p->buf) {
        kheap::free(p);
        return Done::fail(Status::NoMemory);
    }
    p->mask = cap - 1;
    p->open_ends = 2;

    new (&p->read_end) PipeEnd();
    new (&p->write_end) PipeEnd();
    p->read_end.pipe = p;
    p->read_end.reader = true;
    p->write_end.pipe = p;
    p->write_end.reader = false;

    Result<Lens> r = alloc_slot(d, nullptr, nebula::Node{0, 0}, nebula::Node{0, 0}, Right::Read | Right::Delegate, false, 0, "pipe", &p->read_end);
    if (!r.ok()) {
        kheap::free(p->buf);
        kheap::free(p);
        return Done::fail(r.status());
    }
    Result<Lens> w = alloc_slot(d, nullptr, nebula::Node{0, 0}, nebula::Node{0, 0}, Right::Write | Right::Append | Right::Delegate, false, 0, "pipe", &p->write_end);
    if (!w.ok()) {

        (void)close(d, r.value());
        if (__atomic_sub_fetch(&p->open_ends, 1, __ATOMIC_ACQ_REL) == 0) {
            kheap::free(p->buf);
            kheap::free(p);
        }
        return Done::fail(w.status());
    }
    *read_end = r.value();
    *write_end = w.value();
    return Done::success();
}

Result<u32> acquire(Domain& d, Wait* waits, u32 count, Stream** streams) {
    if (count == 0 || count > MAX_WAITS || !waits || !streams) return Result<u32>::fail(Status::InvalidPath);
    u32 pinned = 0;
    for (u32 i = 0; i < count; ++i) {
        waits[i].got = Readiness();
        Result<Slot*> sr = slot_of(d, waits[i].lens);
        Status failure = Status::Ok;
        if (!sr.ok()) failure = sr.status();
        else if (sr.value()->kind != NodeKind::Stream) failure = Status::Unsupported;
        else if (waits[i].want.has(Ready::Readable) && !sr.value()->rights.has(Right::Read)) failure = Status::Denied;
        else if (waits[i].want.has(Ready::Writable) && !sr.value()->rights.has_any(Right::Write | Right::Append)) failure = Status::Denied;
        if (failure != Status::Ok) {
            unpin(streams, pinned);
            return Result<u32>::fail(failure);
        }
        streams[i] = sr.value()->stream;
        streams[i]->retain();
        ++pinned;
    }
    return Result<u32>::success(count);
}

void unpin(Stream** streams, u32 count) {
    for (u32 i = 0; i < count; ++i) streams[i]->release();
}

Result<u32> await_streams(Stream** streams, Wait* waits, u32 count, u64 timeout_ms, CancelFn cancel, void* cancel_ctx) {
    AwaitCtx ctx{waits, streams, count, cancel, cancel_ctx};
    if (!any_ready(&ctx) && timeout_ms != 0) {
        const bool forever = timeout_ms == orbital::FOREVER;
        const u64 deadline = forever ? 0 : deadline_after(clock::uptime_ms(), timeout_ms);
        for (;;) {
            u64 left = orbital::FOREVER;
            if (!forever) {
                u64 now = clock::uptime_ms();
                if (now >= deadline) break;
                left = deadline - now;
            }
            if (g_events.wait_until_for(&any_ready, &ctx, left)) break;
            if (!forever && clock::uptime_ms() >= deadline) break;
        }
    }
    if (cancel && cancel(cancel_ctx)) return Result<u32>::fail(Status::Cancelled);
    u32 ready = 0;
    for (u32 i = 0; i < count; ++i) {
        waits[i].got = filtered(streams[i], waits[i].want);
        if (waits[i].got.any()) ++ready;
    }
    if (ready == 0) return Result<u32>::fail(Status::TimedOut);
    return Result<u32>::success(ready);
}

Result<u32> await(Domain& d, Wait* waits, u32 count, u64 timeout_ms) {
    Stream* streams[MAX_WAITS];
    Result<u32> pinned = acquire(d, waits, count, streams);
    if (!pinned.ok()) return pinned;
    Result<u32> r = await_streams(streams, waits, count, timeout_ms, nullptr, nullptr);
    unpin(streams, count);
    return r;
}

}
