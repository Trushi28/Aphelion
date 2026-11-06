#include <cosmos/lens_channel.hpp>
#include <cosmos/kheap.hpp>
#include <cosmos/orbital.hpp>

namespace lens::channel {

namespace {

struct Channel;

struct Message {
    u32 bytes;
    u32 handles;
    u8 data[MAX_BYTES];
    Parcel parcels[MAX_HANDLES];
};

class End final : public Stream {
public:
    Channel* ch;
    u32 side;
    u32 refs;

    const char* name() const override { return "channel"; }
    Result<usize> read(Bytes) override { return Result<usize>::fail(Status::Unsupported); }
    Result<usize> write(ConstBytes) override { return Result<usize>::fail(Status::Unsupported); }
    Readiness ready() override;
    void retain() override { __atomic_add_fetch(&refs, 1, __ATOMIC_ACQ_REL); }
    void release() override;
    u32 type() const override { return STREAM_TYPE; }
};

struct Channel {
    orbital::Mutex mu;
    Message* slots[2][QUEUE_DEPTH];
    u32 head[2];
    u32 count[2];
    u32 closed[2];
    u32 open_ends;
    End ends[2];
};

struct Guard {
    orbital::Mutex& m;
    explicit Guard(orbital::Mutex& x) : m(x) { m.lock(); }
    ~Guard() { m.unlock(); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

u32 load(const u32* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
void store(u32* p, u32 v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

Readiness End::ready() {
    Channel& c = *ch;
    const u32 peer = side ^ 1;
    const bool gone = load(&c.closed[peer]) != 0;
    Readiness r;
    if (load(&c.count[side]) > 0 || gone) r = r | Ready::Readable;
    if (gone) r = r | Ready::Hangup;
    else if (load(&c.count[peer]) < QUEUE_DEPTH) r = r | Ready::Writable;
    return r;
}

void drop_message(Message* m) {
    for (u32 i = 0; i < m->handles; ++i) discard(m->parcels[i]);
    kheap::free(m);
}

void End::release() {
    if (__atomic_sub_fetch(&refs, 1, __ATOMIC_ACQ_REL) != 0) return;
    Channel* c = ch;
    Message* doomed[QUEUE_DEPTH];
    u32 n = 0;
    {
        Guard hold(c->mu);
        store(&c->closed[side], 1);
        u32 count = c->count[side];
        for (u32 i = 0; i < count; ++i) doomed[n++] = c->slots[side][(c->head[side] + i) % QUEUE_DEPTH];
        c->head[side] = 0;
        store(&c->count[side], 0);
    }
    stream_event();
    for (u32 i = 0; i < n; ++i) drop_message(doomed[i]);
    if (__atomic_sub_fetch(&c->open_ends, 1, __ATOMIC_ACQ_REL) == 0) kheap::free(c);
}

struct Pin {
    End* e = nullptr;
    ~Pin() { if (e) e->release(); }
};

Result<End*> resolve(Domain& d, Lens l, Right need) {
    Result<Slot*> sr = slot_of(d, l);
    if (!sr.ok()) return Result<End*>::fail(sr.status());
    Slot* s = sr.value();
    if (s->kind != NodeKind::Stream || !s->stream || s->stream->type() != STREAM_TYPE)
        return Result<End*>::fail(Status::Unsupported);
    if (!s->rights.has(need)) return Result<End*>::fail(Status::Denied);
    return Result<End*>::success(static_cast<End*>(s->stream));
}

}

Done create(Table& t, Lens* a, Lens* b) {
    *a = Lens();
    *b = Lens();
    Guard guard(t.lock);
    if (free_slots(t.domain) < 2) return Done::fail(Status::TableFull);
    Channel* c = static_cast<Channel*>(kheap::alloc_zeroed(sizeof(Channel)));
    if (!c) return Done::fail(Status::NoMemory);
    new (&c->mu) orbital::Mutex();
    c->open_ends = 2;
    for (u32 i = 0; i < 2; ++i) {
        new (&c->ends[i]) End();
        c->ends[i].ch = c;
        c->ends[i].side = i;
        c->ends[i].refs = 0;
    }
    Result<Lens> ra = alloc_slot(t.domain, nullptr, nebula::Node{0, 0}, nebula::Node{0, 0}, END_RIGHTS, false, 0, "channel", &c->ends[0]);
    if (!ra.ok()) {
        kheap::free(c);
        return Done::fail(ra.status());
    }
    Result<Lens> rb = alloc_slot(t.domain, nullptr, nebula::Node{0, 0}, nebula::Node{0, 0}, END_RIGHTS, false, 0, "channel", &c->ends[1]);
    if (!rb.ok()) {
        (void)close(t.domain, ra.value());
        if (__atomic_sub_fetch(&c->open_ends, 1, __ATOMIC_ACQ_REL) == 0) kheap::free(c);
        return Done::fail(rb.status());
    }
    *a = ra.value();
    *b = rb.value();
    return Done::success();
}

Done send(Table& t, Lens ch, ConstBytes data, const Lens* handles, u32 count) {
    if (data.len > MAX_BYTES || count > MAX_HANDLES || (count && !handles)) return Done::fail(Status::TooLarge);
    Message* m = static_cast<Message*>(kheap::alloc_zeroed(sizeof(Message)));
    if (!m) return Done::fail(Status::NoMemory);
    Status failure = Status::Ok;
    u32 moved = 0;
    {
        Guard guard(t.lock);
        Result<End*> er = resolve(t.domain, ch, Right::Write);
        if (!er.ok()) {
            kheap::free(m);
            return Done::fail(er.status());
        }
        End* e = er.value();
        for (u32 i = 0; i < count && failure == Status::Ok; ++i) {
            for (u32 j = 0; j < i; ++j)
                if (handles[j] == handles[i]) failure = Status::Busy;
            if (failure != Status::Ok) break;
            Result<Slot*> sr = slot_of(t.domain, handles[i]);
            if (!sr.ok()) { failure = sr.status(); break; }
            Slot* s = sr.value();
            if (!s->rights.has(Right::Delegate)) { failure = Status::Denied; break; }
            if (s->stream && s->stream->type() == STREAM_TYPE && static_cast<End*>(s->stream)->ch == e->ch)
                failure = Status::Unsupported;
        }
        if (failure == Status::Ok) {
            Channel* c = e->ch;
            const u32 inbox = e->side ^ 1;
            Guard hold(c->mu);
            if (load(&c->closed[inbox])) failure = Status::Closed;
            else if (load(&c->count[inbox]) >= QUEUE_DEPTH) failure = Status::NotReady;
            else {
                for (u32 i = 0; i < count; ++i) {
                    Result<Slot*> sr = slot_of(t.domain, handles[i]);
                    Done moved_ok = detach(t.domain, handles[i], sr.value()->rights, &m->parcels[i]);
                    if (!moved_ok.ok()) { failure = moved_ok.status(); break; }
                    ++moved;
                }
                if (failure != Status::Ok) {
                    for (u32 i = 0; i < moved; ++i) (void)attach(t.domain, m->parcels[i]);
                } else {
                    m->bytes = static_cast<u32>(data.len);
                    m->handles = count;
                    for (u32 i = 0; i < data.len; ++i) m->data[i] = data.ptr[i];
                    c->slots[inbox][(c->head[inbox] + c->count[inbox]) % QUEUE_DEPTH] = m;
                    store(&c->count[inbox], c->count[inbox] + 1);
                }
            }
        }
    }
    if (failure != Status::Ok) {
        m->handles = moved;
        for (u32 i = 0; i < moved; ++i) discard(m->parcels[i]);
        kheap::free(m);
        return Done::fail(failure);
    }
    stream_event();
    return Done::success();
}

Result<Received> receive(Table& t, Lens ch, Bytes data, Lens* handles, u32 handle_cap, bool block, CancelFn cancel, void* cancel_ctx) {
    Pin pin;
    for (;;) {
        {
            Guard guard(t.lock);
            Result<End*> er = resolve(t.domain, ch, Right::Read);
            if (!er.ok()) return Result<Received>::fail(er.status());
            End* e = er.value();
            if (!pin.e) {
                e->retain();
                pin.e = e;
            }
            Channel* c = e->ch;
            Message* m = nullptr;
            {
                Guard hold(c->mu);
                if (load(&c->count[e->side]) == 0) {
                    if (load(&c->closed[e->side ^ 1])) return Result<Received>::fail(Status::Closed);
                    if (!block) return Result<Received>::fail(Status::NotReady);
                } else {
                    Message* front = c->slots[e->side][c->head[e->side]];
                    if (front->bytes > data.len || front->handles > handle_cap) return Result<Received>::fail(Status::TooLarge);
                    if (front->handles && !handles) return Result<Received>::fail(Status::TooLarge);
                    if (free_slots(t.domain) < front->handles) return Result<Received>::fail(Status::TableFull);
                    m = front;
                    c->head[e->side] = (c->head[e->side] + 1) % QUEUE_DEPTH;
                    store(&c->count[e->side], c->count[e->side] - 1);
                }
            }
            if (m) {
                Received got{ m->bytes, m->handles };
                for (u32 i = 0; i < m->bytes; ++i) data.ptr[i] = m->data[i];
                Status failure = Status::Ok;
                u32 attached = 0;
                for (u32 i = 0; i < m->handles; ++i) {
                    Result<Lens> lr = attach(t.domain, m->parcels[i]);
                    if (!lr.ok()) { failure = lr.status(); break; }
                    handles[i] = lr.value();
                    ++attached;
                }
                if (failure != Status::Ok) {
                    for (u32 i = 0; i < attached; ++i) (void)close(t.domain, handles[i]);
                    for (u32 i = attached; i < m->handles; ++i) discard(m->parcels[i]);
                    m->handles = 0;
                    kheap::free(m);
                    stream_event();
                    return Result<Received>::fail(failure);
                }
                m->handles = 0;
                kheap::free(m);
                stream_event();
                return Result<Received>::success(got);
            }
        }
        Wait w{ ch, Readiness(Ready::Readable), Readiness() };
        Stream* streams[1] = { pin.e };
        Result<u32> waited = await_streams(streams, &w, 1, orbital::FOREVER, cancel, cancel_ctx);
        if (!waited.ok() && waited.status() == Status::Cancelled) return Result<Received>::fail(Status::Cancelled);
    }
}

}
