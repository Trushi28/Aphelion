#pragma once
#include <cosmos/types.hpp>

namespace vmregion {

constexpr u64 PAGE = 0x1000;
constexpr u64 WINDOW_BASE = 0x0000100000000000ull;
constexpr u64 WINDOW_END = 0x00007F0000000000ull;
constexpr u32 MAX_REGIONS = 64;
constexpr u32 MAX_MAPPED_PAGES = 8192;

constexpr u32 PROT_READ = 1u << 0;
constexpr u32 PROT_WRITE = 1u << 1;
constexpr u32 PROT_EXEC = 1u << 2;
constexpr u32 PROT_ALL = PROT_READ | PROT_WRITE | PROT_EXEC;

enum class Status : u8 {
    Ok,
    Invalid,
    Denied,
    Exists,
    Limit,
    NotMapped,
};

constexpr u64 PTE_WRITABLE = 1ull << 1;
constexpr u64 PTE_NO_EXECUTE = 1ull << 63;

constexpr u64 pte_flags(u32 prot) {
    u64 flags = 0;
    if ((prot & PROT_WRITE) != 0) flags |= PTE_WRITABLE;
    if ((prot & PROT_EXEC) == 0) flags |= PTE_NO_EXECUTE;
    return flags;
}

constexpr Status check_prot(u32 prot, u32 max) {
    if ((prot & ~PROT_ALL) != 0 || (prot & PROT_READ) == 0) return Status::Invalid;
    if ((prot & PROT_WRITE) != 0 && (prot & PROT_EXEC) != 0) return Status::Denied;
    if ((prot & ~max) != 0) return Status::Denied;
    return Status::Ok;
}

constexpr bool range_ok(u64 base, u64 pages) {
    if (pages == 0 || pages > MAX_MAPPED_PAGES) return false;
    if ((base & (PAGE - 1)) != 0 || base < WINDOW_BASE || base >= WINDOW_END) return false;
    return pages * PAGE <= WINDOW_END - base;
}

struct Region {
    u64 base;
    u32 pages;
    u32 offset;
    u32 prot;
    u32 max;
    void* object;

    constexpr u64 end() const { return base + static_cast<u64>(pages) * PAGE; }
};

constexpr bool mergeable(const Region& a, const Region& b) {
    return a.object == b.object && a.end() == b.base && a.offset + a.pages == b.offset && a.prot == b.prot && a.max == b.max;
}

template <u32 N = MAX_REGIONS>
class Map {
public:
    static constexpr u32 SLACK = 2;

    constexpr Map() : n_(0), pages_(0), r_() {}

    constexpr u32 count() const { return n_; }
    constexpr u32 mapped_pages() const { return pages_; }
    constexpr const Region& at(u32 i) const { return r_[i]; }

    constexpr const Region* find(u64 addr) const {
        for (u32 i = 0; i < n_; ++i) {
            if (addr < r_[i].base) return nullptr;
            if (addr < r_[i].end()) return &r_[i];
        }
        return nullptr;
    }

    constexpr Status find_gap(u32 pages, u64* out) const {
        if (!range_ok(WINDOW_BASE, pages)) return Status::Invalid;
        u64 need = static_cast<u64>(pages) * PAGE;
        u64 cursor = WINDOW_BASE;
        for (u32 i = 0; i < n_; ++i) {
            if (r_[i].base >= cursor && r_[i].base - cursor >= need) {
                *out = cursor;
                return Status::Ok;
            }
            if (r_[i].end() > cursor) cursor = r_[i].end();
        }
        if (WINDOW_END - cursor >= need) {
            *out = cursor;
            return Status::Ok;
        }
        return Status::Limit;
    }

    constexpr Status check_free(u64 base, u32 pages) const {
        if (!range_ok(base, pages)) return Status::Invalid;
        u64 end = base + static_cast<u64>(pages) * PAGE;
        for (u32 i = 0; i < n_; ++i) {
            if (r_[i].base >= end) break;
            if (r_[i].end() > base) return Status::Exists;
        }
        return Status::Ok;
    }

    constexpr Status insert(const Region& r) {
        Status s = check_free(r.base, r.pages);
        if (s != Status::Ok) return s;
        if (check_prot(r.prot, r.max) != Status::Ok) return Status::Invalid;
        if (n_ >= N || pages_ + r.pages > MAX_MAPPED_PAGES) return Status::Limit;
        u32 at = 0;
        while (at < n_ && r_[at].base < r.base) ++at;
        put(at, r);
        pages_ += r.pages;
        return Status::Ok;
    }

    constexpr bool covered(u64 base, u32 pages) const {
        if (!range_ok(base, pages)) return false;
        u64 end = base + static_cast<u64>(pages) * PAGE;
        u64 cursor = base;
        for (u32 i = 0; i < n_ && cursor < end; ++i) {
            if (r_[i].end() <= cursor) continue;
            if (r_[i].base > cursor) return false;
            cursor = r_[i].end();
        }
        return cursor >= end;
    }

    template <typename Ops>
    constexpr Status unmap(u64 base, u32 pages, Ops& ops) {
        if (!range_ok(base, pages)) return Status::Invalid;
        if (!covered(base, pages)) return Status::NotMapped;
        u64 end = base + static_cast<u64>(pages) * PAGE;
        const Region* first = find(base);
        if (first->base < base && first->end() > end && n_ >= N) return Status::Limit;
        split_at(base, ops);
        split_at(end, ops);
        u32 i = 0;
        while (i < n_ && r_[i].base < base) ++i;
        while (i < n_ && r_[i].base < end) {
            ops.release(r_[i].object);
            erase(i);
        }
        pages_ -= pages;
        return Status::Ok;
    }

    template <typename Ops>
    constexpr Status protect(u64 base, u32 pages, u32 prot, Ops& ops) {
        if (!range_ok(base, pages)) return Status::Invalid;
        Status pv = check_prot(prot, PROT_ALL);
        if (pv != Status::Ok) return pv;
        if (!covered(base, pages)) return Status::NotMapped;
        u64 end = base + static_cast<u64>(pages) * PAGE;
        u32 extra = 0;
        for (u32 i = 0; i < n_; ++i) {
            if (r_[i].base >= end) break;
            if (r_[i].end() <= base) continue;
            if ((prot & ~r_[i].max) != 0) return Status::Denied;
            if (r_[i].base < base) ++extra;
            if (r_[i].end() > end) ++extra;
        }
        if (n_ + extra > N) return Status::Limit;
        split_at(base, ops);
        split_at(end, ops);
        for (u32 i = 0; i < n_; ++i) {
            if (r_[i].base >= end) break;
            if (r_[i].base >= base) r_[i].prot = prot;
        }
        coalesce(ops);
        return Status::Ok;
    }

    template <typename Ops>
    constexpr void clear(Ops& ops) {
        for (u32 i = 0; i < n_; ++i) ops.release(r_[i].object);
        n_ = 0;
        pages_ = 0;
    }

    constexpr bool consistent() const {
        u64 total = 0;
        for (u32 i = 0; i < n_; ++i) {
            const Region& g = r_[i];
            if (g.pages == 0 || !range_ok(g.base, g.pages) || !g.object) return false;
            if (check_prot(g.prot, g.max) != Status::Ok) return false;
            if (i && r_[i - 1].end() > g.base) return false;
            total += g.pages;
        }
        return total == pages_ && pages_ <= MAX_MAPPED_PAGES && n_ <= N;
    }

private:
    u32 n_;
    u32 pages_;
    Region r_[N + SLACK];

    constexpr void put(u32 at, const Region& r) {
        for (u32 i = n_; i > at; --i) r_[i] = r_[i - 1];
        r_[at] = r;
        ++n_;
    }

    constexpr void erase(u32 at) {
        for (u32 i = at; i + 1 < n_; ++i) r_[i] = r_[i + 1];
        --n_;
    }

    template <typename Ops>
    constexpr void split_at(u64 addr, Ops& ops) {
        for (u32 i = 0; i < n_; ++i) {
            if (r_[i].base >= addr) return;
            if (r_[i].end() <= addr) continue;
            u32 cut = static_cast<u32>((addr - r_[i].base) / PAGE);
            Region right = r_[i];
            right.base = addr;
            right.offset += cut;
            right.pages -= cut;
            r_[i].pages = cut;
            ops.retain(right.object);
            put(i + 1, right);
            return;
        }
    }

    template <typename Ops>
    constexpr void coalesce(Ops& ops) {
        u32 i = 0;
        while (i + 1 < n_) {
            if (mergeable(r_[i], r_[i + 1])) {
                r_[i].pages += r_[i + 1].pages;
                ops.release(r_[i + 1].object);
                erase(i + 1);
            } else {
                ++i;
            }
        }
    }
};

}
