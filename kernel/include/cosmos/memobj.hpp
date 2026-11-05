#pragma once
#include <cosmos/lens.hpp>

namespace memobj {

constexpr u32 STREAM_TYPE = 0x4F4D454D;
constexpr u32 MAX_PAGES = 4096;
constexpr u64 GLOBAL_PAGE_LIMIT = 16384;
constexpr u64 POISON_BIT = 1;
constexpr u64 FRAME_MASK = ~0xFFFull;

constexpr lens::Rights HANDLE_RIGHTS = lens::Right::Read | lens::Right::Write | lens::Right::Execute | lens::Right::Delegate;

class Object final : public lens::Stream {
public:
    u32 pages;
    u32 refs;
    u64* frames;

    const char* name() const override { return "memobj"; }
    lens::Result<usize> read(lens::Bytes) override { return lens::Result<usize>::fail(lens::Status::Unsupported); }
    lens::Result<usize> write(lens::ConstBytes) override { return lens::Result<usize>::fail(lens::Status::Unsupported); }
    lens::Readiness ready() override { return lens::Ready::Readable | lens::Ready::Writable; }
    void retain() override { __atomic_add_fetch(&refs, 1u, __ATOMIC_ACQ_REL); }
    void release() override;
    u32 type() const override { return STREAM_TYPE; }
};

Object* create(u32 pages);
void discard(Object* o);
Object* from_stream(lens::Stream* s);

u64 frame(const Object* o, u32 index);
bool poisoned(const Object* o, u32 index);
bool owns(const Object* o, u64 frame_phys);
bool poison(Object* o, u64 frame_phys);

u64 live_pages();
u64 quarantined_pages();
u64 live_objects();

}
