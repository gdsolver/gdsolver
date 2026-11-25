#pragma once
// FlatSet<T>: a set of T kept as a sorted vector, for the shadow's per-step uid sets (selfshadow.hpp gate / ever / usedRings, buttons.hpp
// RingTouches::now, and the Ctx pointers and per-pass sets that meet them). A std::set allocates a node per element and its head on
// construction, and those sets are emptied and filled again for every stepped child (shadow_state.hpp slUnpack): about 2-3% of a
// 1-thread step in allocations (gd-05 11-13, lv11). The vector keeps its storage across clear().
// Iteration is in ascending order, as std::set's, so every loop over one visits the same elements in the same order.
#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace dp {

template <class T>
class FlatSet {
    std::vector<T> v_;
public:
    using value_type = T;
    using iterator = typename std::vector<T>::const_iterator;
    using const_iterator = iterator;
    FlatSet() = default;
    template <class It> FlatSet(It a, It b) { insert(a, b); }
    iterator begin() const { return v_.cbegin(); }
    iterator end() const { return v_.cend(); }
    size_t size() const { return v_.size(); }
    bool empty() const { return v_.empty(); }
    void clear() { v_.clear(); }
    std::pair<iterator, bool> insert(const T& x) {
        if (v_.empty() || v_.back() < x) { v_.push_back(x); return {v_.cend() - 1, true}; }
        auto it = std::lower_bound(v_.begin(), v_.end(), x);
        if (it != v_.end() && !(x < *it)) return {iterator(it), false};
        it = v_.insert(it, x);
        return {iterator(it), true};
    }
    template <class It> void insert(It a, It b) { for (; a != b; ++a) insert(*a); }
    size_t count(const T& x) const { return std::binary_search(v_.begin(), v_.end(), x) ? 1 : 0; }
    iterator find(const T& x) const {
        auto it = std::lower_bound(v_.cbegin(), v_.cend(), x);
        return (it != v_.cend() && !(x < *it)) ? it : v_.cend();
    }
    size_t erase(const T& x) {
        auto it = std::lower_bound(v_.begin(), v_.end(), x);
        if (it == v_.end() || x < *it) return 0;
        v_.erase(it);
        return 1;
    }
    iterator erase(iterator it) { return v_.erase(it); }
    void swap(FlatSet& o) noexcept { v_.swap(o.v_); }
    bool operator==(const FlatSet& o) const { return v_ == o.v_; }
    bool operator!=(const FlatSet& o) const { return v_ != o.v_; }
};

}  // namespace dp
