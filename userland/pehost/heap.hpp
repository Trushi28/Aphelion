#pragma once
#include <cosmos/types.hpp>

namespace pehost {

class Heap {
public:
    using Map = u8* (*)(u64 bytes);

    static constexpr u64 ALIGN = 16;
    static constexpr u64 HEADER = 16;
    static constexpr u64 MIN_BLOCK = 32;
    static constexpr u64 PAGE = 4096;
    static constexpr u32 MAX_CHUNKS = 8;
    static constexpr u64 FIRST_CHUNK = 256 * 1024;
    static constexpr u64 LARGEST_CHUNK = 2 * 1024 * 1024;
    static constexpr u64 CAP = 8 * 1024 * 1024;

    using Hook = void (*)();

    void set_lock(Hook lock, Hook unlock) {
        lock_ = lock;
        unlock_ = unlock;
    }

    void init(Map map) {
        map_ = map;
        chunk_count_ = 0;
        total_ = 0;
        free_head_ = nullptr;
        next_chunk_ = FIRST_CHUNK;
        live_ = 0;
    }

    void* alloc(u64 bytes) {
        Guard guard(this);
        u64 need = block_for(bytes);
        if (need == 0) return nullptr;
        Block* b = take(need);
        if (!b && grow(need)) b = take(need);
        if (!b) return nullptr;
        ++live_;
        return payload(b);
    }

    bool free(void* p) {
        Guard guard(this);
        Block* b = owned(p);
        if (!b) return false;
        --live_;
        release(b);
        return true;
    }

    void* resize(void* p, u64 bytes) {
        Guard guard(this);
        if (!p) return alloc(bytes);
        Block* b = owned(p);
        if (!b) return nullptr;
        u64 need = block_for(bytes);
        if (need == 0) return nullptr;
        u64 have = size_of(b);
        if (need <= have) {
            shrink(b, need);
            return p;
        }
        Block* n = next_of(b);
        if (!in_use(n) && have + size_of(n) >= need) {
            unlink(n);
            u64 merged = have + size_of(n);
            set_size(b, merged, true, prev_in_use(b));
            Block* after = next_of(b);
            mark_prev(after, true);
            shrink(b, need);
            return p;
        }
        void* q = alloc(bytes);
        if (!q) return nullptr;
        u8* d = static_cast<u8*>(q);
        const u8* s = static_cast<const u8*>(p);
        u64 keep = have - HEADER;
        for (u64 i = 0; i < keep; ++i) d[i] = s[i];
        free(p);
        return q;
    }

    u64 usable(const void* p) const {
        Guard guard(this);
        Block* b = owned(const_cast<void*>(p));
        return b ? size_of(b) - HEADER : 0;
    }

    bool owns(const void* p) const {
        Guard guard(this);
        return owned(const_cast<void*>(p)) != nullptr;
    }

    u64 live() const { return live_; }
    u64 mapped() const { return total_; }
    u32 chunks() const { return chunk_count_; }

    bool check(u64* used_bytes = nullptr, u64* free_bytes = nullptr) const {
        u64 used = 0, spare = 0, live = 0, free_blocks = 0;
        for (u32 i = 0; i < chunk_count_; ++i) {
            const Chunk& c = chunks_[i];
            u8* at = c.base;
            u8* end = c.base + c.bytes - HEADER;
            bool prev_used = true;
            while (at < end) {
                Block* b = reinterpret_cast<Block*>(at);
                u64 size = size_of(b);
                if (size < MIN_BLOCK || at + size > end) return false;
                if (prev_in_use(b) != prev_used) return false;
                if (in_use(b)) {
                    used += size;
                    ++live;
                } else {
                    if (!prev_used) return false;
                    spare += size;
                    ++free_blocks;
                    if (prev_size(next_of(b)) != size) return false;
                }
                prev_used = in_use(b);
                at += size;
            }
            Block* sentinel = reinterpret_cast<Block*>(end);
            if (size_of(sentinel) != 0 || !in_use(sentinel) || prev_in_use(sentinel) != prev_used) return false;
        }
        u64 listed = 0;
        Block* last = nullptr;
        for (Block* b = free_head_; b; b = links(b)->next) {
            if (in_use(b) || links(b)->prev != last) return false;
            if (!inside(b)) return false;
            last = b;
            ++listed;
        }
        if (listed != free_blocks || live != live_) return false;
        if (used_bytes) *used_bytes = used;
        if (free_bytes) *free_bytes = spare;
        return true;
    }

private:
    struct Guard {
        const Heap* heap;
        explicit Guard(const Heap* h) : heap(h) {
            if (heap->lock_) heap->lock_();
        }
        ~Guard() {
            if (heap->unlock_) heap->unlock_();
        }
    };

    struct Block {
        u64 size_flags;
        u64 prev;
    };

    struct Links {
        Block* next;
        Block* prev;
    };

    struct Chunk {
        u8* base;
        u64 bytes;
    };

    static constexpr u64 IN_USE = 1;
    static constexpr u64 PREV_IN_USE = 2;
    static constexpr u64 FLAGS = 15;

    static u64 size_of(const Block* b) { return b->size_flags & ~FLAGS; }
    static bool in_use(const Block* b) { return (b->size_flags & IN_USE) != 0; }
    static bool prev_in_use(const Block* b) { return (b->size_flags & PREV_IN_USE) != 0; }
    static u64 prev_size(const Block* b) { return b->prev; }
    static Block* next_of(const Block* b) { return reinterpret_cast<Block*>(const_cast<u8*>(reinterpret_cast<const u8*>(b)) + size_of(b)); }
    static void* payload(Block* b) { return reinterpret_cast<u8*>(b) + HEADER; }
    static Links* links(Block* b) { return reinterpret_cast<Links*>(reinterpret_cast<u8*>(b) + HEADER); }

    static void set_size(Block* b, u64 size, bool used, bool prev_used) {
        b->size_flags = size | (used ? IN_USE : 0) | (prev_used ? PREV_IN_USE : 0);
    }

    static void mark_prev(Block* b, bool used) {
        if (used) b->size_flags |= PREV_IN_USE;
        else b->size_flags &= ~PREV_IN_USE;
    }

    static u64 block_for(u64 bytes) {
        if (bytes > CAP) return 0;
        u64 need = (bytes + HEADER + ALIGN - 1) & ~(ALIGN - 1);
        return need < MIN_BLOCK ? MIN_BLOCK : need;
    }

    bool inside(const Block* b) const {
        const u8* at = reinterpret_cast<const u8*>(b);
        for (u32 i = 0; i < chunk_count_; ++i) {
            if (at >= chunks_[i].base && at < chunks_[i].base + chunks_[i].bytes - HEADER) return true;
        }
        return false;
    }

    Block* owned(void* p) const {
        u64 at = reinterpret_cast<u64>(p);
        if (at % ALIGN != 0) return nullptr;
        for (u32 i = 0; i < chunk_count_; ++i) {
            u64 lo = reinterpret_cast<u64>(chunks_[i].base) + HEADER;
            u64 hi = reinterpret_cast<u64>(chunks_[i].base) + chunks_[i].bytes - HEADER;
            if (at < lo || at >= hi) continue;
            Block* b = reinterpret_cast<Block*>(at - HEADER);
            if (!in_use(b)) return nullptr;
            u64 size = size_of(b);
            if (size < MIN_BLOCK || (at - HEADER) + size > hi) return nullptr;
            if (!prev_in_use(next_of(b))) return nullptr;
            return b;
        }
        return nullptr;
    }

    void push(Block* b) {
        Links* l = links(b);
        l->prev = nullptr;
        l->next = free_head_;
        if (free_head_) links(free_head_)->prev = b;
        free_head_ = b;
    }

    void unlink(Block* b) {
        Links* l = links(b);
        if (l->prev) links(l->prev)->next = l->next;
        else free_head_ = l->next;
        if (l->next) links(l->next)->prev = l->prev;
    }

    Block* take(u64 need) {
        for (Block* b = free_head_; b; b = links(b)->next) {
            u64 size = size_of(b);
            if (size < need) continue;
            unlink(b);
            bool prev_used = prev_in_use(b);
            if (size - need >= MIN_BLOCK) {
                set_size(b, need, true, prev_used);
                Block* rest = next_of(b);
                set_size(rest, size - need, false, true);
                Block* after = next_of(rest);
                after->prev = size - need;
                mark_prev(after, false);
                push(rest);
            } else {
                set_size(b, size, true, prev_used);
                mark_prev(next_of(b), true);
            }
            return b;
        }
        return nullptr;
    }

    void shrink(Block* b, u64 need) {
        u64 size = size_of(b);
        if (size - need < MIN_BLOCK) return;
        set_size(b, need, true, prev_in_use(b));
        Block* rest = next_of(b);
        set_size(rest, size - need, true, true);
        release(rest);
    }

    void release(Block* b) {
        u64 size = size_of(b);
        bool prev_used = prev_in_use(b);
        Block* n = next_of(b);
        if (!in_use(n)) {
            unlink(n);
            size += size_of(n);
        }
        if (!prev_used) {
            Block* p = reinterpret_cast<Block*>(reinterpret_cast<u8*>(b) - prev_size(b));
            unlink(p);
            size += size_of(p);
            b = p;
            prev_used = prev_in_use(p);
        }
        set_size(b, size, false, prev_used);
        Block* after = next_of(b);
        after->prev = size;
        mark_prev(after, false);
        push(b);
    }

    bool grow(u64 need) {
        if (chunk_count_ == MAX_CHUNKS || !map_) return false;
        u64 bytes = next_chunk_;
        u64 least = (need + HEADER + PAGE - 1) & ~(PAGE - 1);
        if (bytes < least) bytes = least;
        if (total_ + bytes > CAP) {
            bytes = least;
            if (total_ + bytes > CAP) return false;
        }
        u8* base = map_(bytes);
        if (!base || reinterpret_cast<u64>(base) % ALIGN != 0) return false;
        chunks_[chunk_count_++] = Chunk{ base, bytes };
        total_ += bytes;
        if (next_chunk_ < LARGEST_CHUNK) next_chunk_ *= 2;
        Block* b = reinterpret_cast<Block*>(base);
        set_size(b, bytes - HEADER, false, true);
        Block* sentinel = next_of(b);
        sentinel->prev = bytes - HEADER;
        set_size(sentinel, 0, true, false);
        push(b);
        return true;
    }

    Map map_ = nullptr;
    Hook lock_ = nullptr;
    Hook unlock_ = nullptr;
    Chunk chunks_[MAX_CHUNKS] = {};
    u32 chunk_count_ = 0;
    u64 total_ = 0;
    u64 next_chunk_ = FIRST_CHUNK;
    u64 live_ = 0;
    Block* free_head_ = nullptr;
};

}
