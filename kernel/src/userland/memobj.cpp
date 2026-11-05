#include <cosmos/memobj.hpp>
#include <cosmos/kheap.hpp>
#include <cosmos/pmm.hpp>

namespace memobj {

namespace {

volatile u64 g_pages_live = 0;
volatile u64 g_pages_quarantined = 0;
volatile u64 g_objects_live = 0;

bool reserve(u32 pages) {
    u64 cur = __atomic_load_n(&g_pages_live, __ATOMIC_RELAXED);
    for (;;) {
        if (cur + pages > GLOBAL_PAGE_LIMIT) return false;
        if (__atomic_compare_exchange_n(&g_pages_live, &cur, cur + pages, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return true;
    }
}

void unreserve(u32 pages) { __atomic_sub_fetch(&g_pages_live, static_cast<u64>(pages), __ATOMIC_ACQ_REL); }

void destroy(Object* o) {
    for (u32 i = 0; i < o->pages; ++i) {
        u64 f = o->frames[i];
        if (!f) continue;
        if (f & POISON_BIT) __atomic_add_fetch(&g_pages_quarantined, 1ull, __ATOMIC_RELAXED);
        else universe::free(f & FRAME_MASK, 0);
    }
    unreserve(o->pages);
    kheap::free(o->frames);
    __atomic_sub_fetch(&g_objects_live, 1ull, __ATOMIC_RELAXED);
    kheap::free(o);
}

}

Object* create(u32 pages) {
    if (pages == 0 || pages > MAX_PAGES) return nullptr;
    if (!reserve(pages)) return nullptr;
    Object* o = static_cast<Object*>(kheap::alloc_zeroed(sizeof(Object)));
    if (!o) {
        unreserve(pages);
        return nullptr;
    }
    new (o) Object();
    o->pages = pages;
    o->refs = 0;
    o->frames = static_cast<u64*>(kheap::alloc_zeroed(static_cast<usize>(pages) * sizeof(u64)));
    if (!o->frames) {
        unreserve(pages);
        kheap::free(o);
        return nullptr;
    }
    for (u32 i = 0; i < pages; ++i) {
        u64 f = universe::alloc_zeroed(0);
        if (!f) {
            for (u32 k = 0; k < i; ++k) universe::free(o->frames[k] & FRAME_MASK, 0);
            unreserve(pages);
            kheap::free(o->frames);
            kheap::free(o);
            return nullptr;
        }
        o->frames[i] = f;
    }
    __atomic_add_fetch(&g_objects_live, 1ull, __ATOMIC_RELAXED);
    return o;
}

void discard(Object* o) {
    if (o && __atomic_load_n(&o->refs, __ATOMIC_ACQUIRE) == 0) destroy(o);
}

void Object::release() {
    if (__atomic_sub_fetch(&refs, 1u, __ATOMIC_ACQ_REL) == 0) destroy(this);
}

Object* from_stream(lens::Stream* s) {
    if (!s || s->type() != STREAM_TYPE) return nullptr;
    return static_cast<Object*>(s);
}

u64 frame(const Object* o, u32 index) {
    if (index >= o->pages) return 0;
    return __atomic_load_n(&o->frames[index], __ATOMIC_ACQUIRE) & FRAME_MASK;
}

bool poisoned(const Object* o, u32 index) {
    if (index >= o->pages) return false;
    return (__atomic_load_n(&o->frames[index], __ATOMIC_ACQUIRE) & POISON_BIT) != 0;
}

bool owns(const Object* o, u64 frame_phys) {
    for (u32 i = 0; i < o->pages; ++i)
        if (frame(o, i) == frame_phys) return true;
    return false;
}

bool poison(Object* o, u64 frame_phys) {
    for (u32 i = 0; i < o->pages; ++i) {
        if (frame(o, i) != frame_phys) continue;
        __atomic_or_fetch(&o->frames[i], POISON_BIT, __ATOMIC_ACQ_REL);
        return true;
    }
    return false;
}

u64 live_pages() { return __atomic_load_n(&g_pages_live, __ATOMIC_RELAXED); }
u64 quarantined_pages() { return __atomic_load_n(&g_pages_quarantined, __ATOMIC_RELAXED); }
u64 live_objects() { return __atomic_load_n(&g_objects_live, __ATOMIC_RELAXED); }

}
