#pragma once
#include <cosmos/types.hpp>

namespace runq {

template <typename Node, int RINGS>
class Queue {
public:
    void push(Node* n) {
        int r = clamp(n->ring);
        n->ring = r;
        n->next = nullptr;
        if (!head_[r]) head_[r] = tail_[r] = n;
        else { tail_[r]->next = n; tail_[r] = n; }
        ++size_;
    }

    Node* pop_highest() {
        for (int r = 0; r < RINGS; ++r) {
            Node* n = head_[r];
            if (!n) continue;
            head_[r] = n->next;
            if (!head_[r]) tail_[r] = nullptr;
            n->next = nullptr;
            --size_;
            return n;
        }
        return nullptr;
    }

    bool remove(Node* n) {
        for (int r = 0; r < RINGS; ++r) {
            Node* prev = nullptr;
            for (Node* it = head_[r]; it; prev = it, it = it->next) {
                if (it != n) continue;
                if (prev) prev->next = it->next;
                else head_[r] = it->next;
                if (tail_[r] == it) tail_[r] = prev;
                it->next = nullptr;
                --size_;
                return true;
            }
        }
        return false;
    }

    template <typename F>
    u32 boost(F&& moved) {
        u32 n = 0;
        for (int r = 1; r < RINGS; ++r) {
            Node* first = head_[r];
            if (!first) continue;
            for (Node* it = first; it; it = it->next) {
                it->ring = 0;
                moved(it);
                ++n;
            }
            if (!head_[0]) head_[0] = first;
            else tail_[0]->next = first;
            tail_[0] = tail_[r];
            head_[r] = tail_[r] = nullptr;
        }
        return n;
    }

    bool empty() const { return size_ == 0; }
    u32 size() const { return size_; }

    int top_ring() const {
        for (int r = 0; r < RINGS; ++r) if (head_[r]) return r;
        return RINGS;
    }

    u32 size_of(int ring) const {
        u32 n = 0;
        for (Node* it = head_[clamp(ring)]; it; it = it->next) ++n;
        return n;
    }

    bool contains(const Node* n) const {
        for (int r = 0; r < RINGS; ++r)
            for (Node* it = head_[r]; it; it = it->next) if (it == n) return true;
        return false;
    }

private:
    static constexpr int clamp(int r) { return r < 0 ? 0 : (r >= RINGS ? RINGS - 1 : r); }

    Node* head_[RINGS] = {};
    Node* tail_[RINGS] = {};
    u32 size_ = 0;
};

}
