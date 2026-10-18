#include <cosmos/kheap.hpp>
#include <cosmos/pmm.hpp>
#include <cosmos/serial.hpp>
#include <cosmos/cpu.hpp>

namespace kheap {

namespace {

constexpr u32 SIZES[CLASS_COUNT] = { 16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048 };
constexpr usize HEADER = 64;
constexpr u32 MIN_OBJECTS = 4;
constexpr u8 POISON = 0xDD;
constexpr u64 FREE_MAGIC = 0xF4EE0B1EC7ADEAD5ull;
constexpr u32 INFO_LARGE_HEAD = 0x80000000u;
constexpr u32 INFO_LARGE_TAIL = 0x40000000u;
constexpr u32 INFO_PAGES_MASK = 0x3FFFFFFFu;
constexpr u64 PAGE = universe::PAGE_SIZE;
constexpr u64 MAX_LARGE_BYTES = PAGE << universe::MAX_ORDER;

struct FreeObject {
    u64 magic;
    FreeObject* next;
};

struct Slab {
    Slab* next;
    Slab* prev;
    FreeObject* head;
    u32 inuse;
    u32 capacity;
    u32 cls;
};

static_assert(sizeof(Slab) <= HEADER, "slab header must fit its reserved space");
static_assert(sizeof(FreeObject) == 16, "the smallest class must hold the free-list link");

struct Class {
    cpu::Spinlock lock;
    Slab* partial;
    u32 order;
    u32 stride;
    u32 capacity;
    u32 slabs;
    u64 live;
    u64 allocs;
    u64 frees;
};

struct Large {
    cpu::Spinlock lock;
    u64 blocks;
    u64 pages;
    u64 allocs;
    u64 frees;
};

Class g_class[CLASS_COUNT];
Large g_large;
u8 g_lookup[129];
u32* g_info = nullptr;
u64 g_info_pages = 0;
volatile u64 g_failed = 0;
volatile bool g_ready = false;

struct Guard {
    cpu::Spinlock& lock;
    u64 flags;
    explicit Guard(cpu::Spinlock& l) : lock(l), flags(cpu::irq_save()) { lock.lock(); }
    ~Guard() { lock.unlock(); cpu::irq_restore(flags); }
};

NORETURN void fatal(const char* what, const void* p) {
    serial::printf("[kheap] FATAL: %s (pointer %p)\n", what, reinterpret_cast<u64>(p));
    cpu::hang();
}

inline u64 slab_bytes(const Class& k) { return PAGE << k.order; }

void release_pages(u64 phys, u64 first, u64 count) {
    u64 p = first;
    u64 end = first + count;
    while (p < end) {
        int k = 0;
        while (k < universe::MAX_ORDER && (p & ((2ull << k) - 1)) == 0 && p + (2ull << k) <= end) ++k;
        universe::free(phys + p * PAGE, k);
        p += 1ull << k;
    }
}

void poison_free_object(FreeObject* o, u32 stride, FreeObject* next) {
    __builtin_memset(o, POISON, stride);
    o->magic = FREE_MAGIC ^ reinterpret_cast<u64>(o);
    o->next = next;
}

bool object_intact(const FreeObject* o, const Slab* s, const Class& k) {
    if (!o) return false;
    if (o->magic != (FREE_MAGIC ^ reinterpret_cast<u64>(o))) return false;
    u64 base = reinterpret_cast<u64>(s);
    u64 n = reinterpret_cast<u64>(o->next);
    if (n && (n < base + HEADER || n >= base + slab_bytes(k))) return false;
    const u8* b = reinterpret_cast<const u8*>(o);
    for (u32 i = sizeof(FreeObject); i < k.stride; ++i)
        if (b[i] != POISON) return false;
    return true;
}

Slab* new_slab(u32 c) {
    Class& k = g_class[c];
    u64 phys = universe::alloc(static_cast<int>(k.order));
    if (!phys) return nullptr;
    auto* s = static_cast<Slab*>(universe::to_virt(phys));
    u8* objects = reinterpret_cast<u8*>(s) + HEADER;
    s->next = nullptr;
    s->prev = nullptr;
    s->inuse = 0;
    s->capacity = k.capacity;
    s->cls = c;
    FreeObject* next = nullptr;
    for (u32 i = k.capacity; i-- > 0;) {
        auto* o = reinterpret_cast<FreeObject*>(objects + static_cast<u64>(i) * k.stride);
        poison_free_object(o, k.stride, next);
        next = o;
    }
    s->head = next;
    u64 first = phys >> 12;
    for (u64 i = 0; i < (1ull << k.order); ++i) g_info[first + i] = c + 1;
    ++k.slabs;
    return s;
}

void unlink_partial(Class& k, Slab* s) {
    if (s->prev) s->prev->next = s->next;
    else k.partial = s->next;
    if (s->next) s->next->prev = s->prev;
    s->next = nullptr;
    s->prev = nullptr;
}

void push_partial(Class& k, Slab* s) {
    s->prev = nullptr;
    s->next = k.partial;
    if (k.partial) k.partial->prev = s;
    k.partial = s;
}

void* alloc_small(u32 c) {
    Class& k = g_class[c];
    Guard g(k.lock);
    Slab* s = k.partial;
    if (!s) {
        s = new_slab(c);
        if (!s) return nullptr;
        push_partial(k, s);
    }
    FreeObject* o = s->head;
    if (!object_intact(o, s, k)) fatal("free object damaged: write after free or heap corruption", o);
    s->head = o->next;
    ++s->inuse;
    if (!s->head) unlink_partial(k, s);
    ++k.live;
    ++k.allocs;
    o->magic = 0;
    o->next = nullptr;
    return o;
}

void free_small(void* p, u32 c) {
    Class& k = g_class[c];
    u64 phys = universe::to_phys(p);
    u64 base_phys = phys & ~(slab_bytes(k) - 1);
    auto* s = static_cast<Slab*>(universe::to_virt(base_phys));
    u64 off = phys - base_phys;
    if (off < HEADER || (off - HEADER) % k.stride != 0 || (off - HEADER) / k.stride >= k.capacity)
        fatal("free of a pointer that is not the start of an object", p);

    Guard g(k.lock);
    if (s->cls != c) fatal("slab header does not match the page map", p);
    auto* o = static_cast<FreeObject*>(p);
    if (o->magic == (FREE_MAGIC ^ reinterpret_cast<u64>(o))) fatal("double free", p);
    bool was_full = s->head == nullptr;
    poison_free_object(o, k.stride, s->head);
    s->head = o;
    --s->inuse;
    --k.live;
    ++k.frees;
    if (was_full) push_partial(k, s);
    if (s->inuse == 0 && (s->prev || s->next)) {
        unlink_partial(k, s);
        u64 first = base_phys >> 12;
        for (u64 i = 0; i < (1ull << k.order); ++i) g_info[first + i] = 0;
        --k.slabs;
        universe::free(base_phys, static_cast<int>(k.order));
    }
}

void* alloc_large(usize bytes) {
    u64 npages = (bytes + PAGE - 1) / PAGE;
    int order = 0;
    while ((1ull << order) < npages) ++order;
    u64 phys = universe::alloc(order);
    if (!phys) return nullptr;
    if (npages < (1ull << order)) release_pages(phys, npages, (1ull << order) - npages);
    u64 first = phys >> 12;
    g_info[first] = INFO_LARGE_HEAD | static_cast<u32>(npages);
    for (u64 i = 1; i < npages; ++i) g_info[first + i] = INFO_LARGE_TAIL;
    Guard g(g_large.lock);
    ++g_large.blocks;
    g_large.pages += npages;
    ++g_large.allocs;
    return universe::to_virt(phys);
}

void free_large(void* p, u32 info) {
    u64 phys = universe::to_phys(p);
    if (phys & (PAGE - 1)) fatal("free of a pointer into the first page of a large block", p);
    u64 first = phys >> 12;
    u64 npages = info & INFO_PAGES_MASK;
    {
        Guard g(g_large.lock);
        if (__atomic_load_n(&g_info[first], __ATOMIC_RELAXED) != info) fatal("double free of a large block", p);
        g_info[first] = 0;
        --g_large.blocks;
        g_large.pages -= npages;
        ++g_large.frees;
    }
    for (u64 i = 1; i < npages; ++i) {
        if (g_info[first + i] != INFO_LARGE_TAIL) fatal("large block page map damaged", p);
        g_info[first + i] = 0;
    }
    release_pages(phys, 0, npages);
}

u32 info_of(const void* p) {
    u64 phys = universe::to_phys(p);
    if (phys >= (g_info_pages << 12)) fatal("pointer is outside the direct map", p);
    u32 v = __atomic_load_n(&g_info[phys >> 12], __ATOMIC_RELAXED);
    if (v == 0) fatal("pointer was never allocated or is already freed", p);
    if (v & INFO_LARGE_TAIL) fatal("pointer into the middle of a large block", p);
    return v;
}

}

void init() {
    if (g_ready) return;
    u64 pages = universe::page_count();
    u64 bytes = pages * sizeof(u32);
    int order = 0;
    while ((PAGE << order) < bytes) ++order;
    if (order > universe::MAX_ORDER) {
        serial::writeln("[kheap] FATAL: page map does not fit one buddy block");
        cpu::hang();
    }
    u64 phys = universe::alloc_zeroed(order);
    if (!phys) {
        serial::writeln("[kheap] FATAL: no memory for the page map");
        cpu::hang();
    }
    u64 needed = (bytes + PAGE - 1) / PAGE;
    if (needed < (1ull << order)) release_pages(phys, needed, (1ull << order) - needed);
    g_info = static_cast<u32*>(universe::to_virt(phys));
    g_info_pages = pages;

    for (u32 c = 0; c < CLASS_COUNT; ++c) {
        Class& k = g_class[c];
        k.stride = SIZES[c];
        k.order = 0;
        while (((PAGE << k.order) - HEADER) / k.stride < MIN_OBJECTS) ++k.order;
        k.capacity = static_cast<u32>(((PAGE << k.order) - HEADER) / k.stride);
    }
    u32 c = 0;
    for (u32 i = 0; i <= 128; ++i) {
        while (SIZES[c] < i * 16) ++c;
        g_lookup[i] = static_cast<u8>(c);
    }
    __atomic_store_n(&g_ready, true, __ATOMIC_RELEASE);
    serial::printf("[kheap] online: %u size classes 16..%lu B, large blocks straight from the buddy allocator, page map %lu KiB\n",
                    CLASS_COUNT, static_cast<u64>(MAX_SMALL), (needed * PAGE) / 1024);
}

bool ready() { return __atomic_load_n(&g_ready, __ATOMIC_ACQUIRE); }

void* alloc(usize bytes) {
    if (!ready()) fatal("allocation before kheap::init", nullptr);
    void* p;
    if (bytes <= MAX_SMALL) p = alloc_small(g_lookup[(bytes + 15) / 16]);
    else if (bytes > MAX_LARGE_BYTES) p = nullptr;
    else p = alloc_large(bytes);
    if (!p) __atomic_fetch_add(&g_failed, 1ull, __ATOMIC_RELAXED);
    return p;
}

void* alloc_zeroed(usize bytes) {
    void* p = alloc(bytes);
    if (p) __builtin_memset(p, 0, bytes);
    return p;
}

void free(void* p) {
    if (!p) return;
    u32 v = info_of(p);
    if (v & INFO_LARGE_HEAD) free_large(p, v);
    else free_small(p, v - 1);
}

usize usable_size(const void* p) {
    if (!p) return 0;
    u32 v = info_of(p);
    if (v & INFO_LARGE_HEAD) return static_cast<usize>(v & INFO_PAGES_MASK) * PAGE;
    return g_class[v - 1].stride;
}

Stats stats() {
    Stats s{};
    for (u32 c = 0; c < CLASS_COUNT; ++c) {
        Class& k = g_class[c];
        Guard g(k.lock);
        s.live_objects += k.live;
        s.live_bytes += k.live * k.stride;
        s.slab_pages += static_cast<u64>(k.slabs) << k.order;
        s.alloc_calls += k.allocs;
        s.free_calls += k.frees;
    }
    {
        Guard g(g_large.lock);
        s.live_objects += g_large.blocks;
        s.live_bytes += g_large.pages * PAGE;
        s.large_blocks = g_large.blocks;
        s.large_pages = g_large.pages;
        s.alloc_calls += g_large.allocs;
        s.free_calls += g_large.frees;
    }
    s.failed_allocs = __atomic_load_n(&g_failed, __ATOMIC_RELAXED);
    return s;
}

bool check() {
    bool ok = true;
    for (u32 c = 0; c < CLASS_COUNT && ok; ++c) {
        Class& k = g_class[c];
        Guard g(k.lock);
        u64 guard_steps = 0;
        Slab* prev = nullptr;
        for (Slab* s = k.partial; s && ok; prev = s, s = s->next) {
            if (++guard_steps > (1ull << 32)) { ok = false; break; }
            if (s->prev != prev || s->cls != c || s->capacity != k.capacity || s->inuse >= s->capacity) { ok = false; break; }
            u32 listed = 0;
            for (FreeObject* o = s->head; o; o = o->next) {
                if (!object_intact(o, s, k) || ++listed > s->capacity) { ok = false; break; }
            }
            if (ok && listed != s->capacity - s->inuse) ok = false;
            u64 first = universe::to_phys(s) >> 12;
            for (u64 i = 0; ok && i < (1ull << k.order); ++i)
                if (g_info[first + i] != c + 1) ok = false;
        }
    }
    return ok;
}

bool selftest() {
    static void* held[64];
    static u32 sizes[64];
    Stats before = stats();
    bool ok = check();

    static const u32 probe[] = { 0, 1, 15, 16, 17, 31, 33, 48, 95, 97, 200, 255, 256, 500, 1000, 1500, 2047, 2048,
                                 2049, 4096, 4097, 5000, 12288, 70000, 3u << 20 };
    constexpr u32 N = sizeof(probe) / sizeof(probe[0]);
    for (u32 i = 0; i < N && ok; ++i) {
        void* p = alloc(probe[i]);
        if (!p) { ok = false; break; }
        u64 a = reinterpret_cast<u64>(p);
        if (a % GUARANTEED_ALIGNMENT) ok = false;
        if (probe[i] > MAX_SMALL && (a & (PAGE - 1))) ok = false;
        if (usable_size(p) < probe[i]) ok = false;
        __builtin_memset(p, static_cast<int>(i + 1), probe[i]);
        held[i] = p;
        sizes[i] = probe[i];
    }
    for (u32 i = 0; i < N && ok; ++i) {
        const u8* b = static_cast<const u8*>(held[i]);
        for (u32 j = 0; j < sizes[i]; ++j) if (b[j] != static_cast<u8>(i + 1)) { ok = false; break; }
    }
    ok = ok && check();
    u64 x = 0x9E3779B97F4A7C15ull;
    for (u32 i = N; i > 1; --i) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        u32 j = static_cast<u32>(x % i);
        void* t = held[i - 1]; held[i - 1] = held[j]; held[j] = t;
        u32 ts = sizes[i - 1]; sizes[i - 1] = sizes[j]; sizes[j] = ts;
    }
    for (u32 i = 0; i < N; ++i) free(held[i]);

    void* dirty = alloc(256);
    if (dirty) { __builtin_memset(dirty, 0xFF, 256); free(dirty); }
    auto* z = static_cast<u8*>(alloc_zeroed(256));
    if (!z) ok = false;
    else {
        for (u32 i = 0; i < 256; ++i) if (z[i]) ok = false;
        free(z);
    }

    Stats after = stats();
    ok = ok && after.live_objects == before.live_objects && after.live_bytes == before.live_bytes
            && after.large_blocks == before.large_blocks && after.large_pages == before.large_pages;
    return ok && check();
}

}
