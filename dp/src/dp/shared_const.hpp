// SharedConst<T>: a container that copies by sharing (copy-on-write).
//
// The trigger shadow (trigshadow.hpp) is copied for every new timeline key of the search -- 7,881 times for the 5,598 layers of one lv22 call -- and most of what a copy used to duplicate is the level's
// static tables (object index, groups, trigger rows, ...), which no step ever writes. Copying them was some forty thousand heap allocations a copy, all of them by the one thread that makes the copies while the
// search's worker threads wait.
//
// A SharedConst reads like the container it holds (begin / end / find / count / size / at / operator[] of a vector) and copies in O(1); the loader, which fills the tables before any copy exists, writes through
// mut(). mut() clones the table first if another SharedConst still shares it, so a write can never reach a copy made earlier; `unshared` counts those clones (a step that writes a table it was meant to only read
// shows up there).
#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace dp {
namespace gdtrig {

inline std::atomic<long long> g_sharedConstClones{0};   // mut() calls that had to clone a shared table

template <class T>
class SharedConst {
public:
    SharedConst() : p_(std::make_shared<T>()) {}
    SharedConst(const T& v) : p_(std::make_shared<T>(v)) {}
    SharedConst(T&& v) : p_(std::make_shared<T>(std::move(v))) {}
    SharedConst& operator=(const T& v) { p_ = std::make_shared<T>(v); return *this; }
    SharedConst& operator=(T&& v) { p_ = std::make_shared<T>(std::move(v)); return *this; }

    const T& get() const { return *p_; }
    const T& operator*() const { return *p_; }
    const T* operator->() const { return p_.get(); }
    operator const T&() const { return *p_; }

    // the table for writing: a private copy first when other timelines still hold it
    T& mut() {
        if (p_.use_count() > 1) { p_ = std::make_shared<T>(*p_); g_sharedConstClones.fetch_add(1, std::memory_order_relaxed); }
        return *p_;
    }

    auto begin() const { return p_->begin(); }
    auto end() const { return p_->end(); }
    size_t size() const { return p_->size(); }
    bool empty() const { return p_->empty(); }
    template <class K> auto find(const K& k) const { return p_->find(k); }
    template <class K> size_t count(const K& k) const { return p_->count(k); }
    template <class K> const auto& at(const K& k) const { return p_->at(k); }
    template <class K> const auto& operator[](const K& k) const { return std::as_const(*p_)[k]; }   // (a vector's; a map is written through mut()[k])
    // writes: each clones first when the table is shared (so a copy made earlier never sees them)
    template <class... A> auto insert(A&&... a) { return mut().insert(std::forward<A>(a)...); }
    template <class... A> auto emplace(A&&... a) { return mut().emplace(std::forward<A>(a)...); }
    template <class... A> auto emplace_back(A&&... a) -> decltype(auto) { return mut().emplace_back(std::forward<A>(a)...); }
    template <class... A> void push_back(A&&... a) { mut().push_back(std::forward<A>(a)...); }
    template <class... A> auto erase(A&&... a) { return mut().erase(std::forward<A>(a)...); }
    template <class... A> void assign(A&&... a) { mut().assign(std::forward<A>(a)...); }
    template <class... A> void reserve(A&&... a) { mut().reserve(std::forward<A>(a)...); }
    void clear() { mut().clear(); }
    const auto& front() const { return p_->front(); }
    const auto& back() const { return p_->back(); }
    const auto* data() const { return p_->data(); }

private:
    std::shared_ptr<T> p_;
};

// IntList: an immutable list of ints that copies by sharing -- an object's group list. A level has thousands of objects and every copy of the shadow copied each one's vector (a heap allocation apiece).
class IntList {
public:
    IntList() : p_(emptyVec()) {}
    IntList(const std::vector<int>& v) : p_(v.empty() ? emptyVec() : std::make_shared<const std::vector<int>>(v)) {}
    IntList(std::vector<int>&& v) : p_(v.empty() ? emptyVec() : std::make_shared<const std::vector<int>>(std::move(v))) {}
    IntList& operator=(const std::vector<int>& v) { return *this = IntList(v); }
    operator const std::vector<int>&() const { return *p_; }
    const std::vector<int>& get() const { return *p_; }
    auto begin() const { return p_->begin(); }
    auto end() const { return p_->end(); }
    size_t size() const { return p_->size(); }
    bool empty() const { return p_->empty(); }
    int operator[](size_t i) const { return (*p_)[i]; }
    int front() const { return p_->front(); }
    int back() const { return p_->back(); }
    const int* data() const { return p_->data(); }
    bool operator==(const IntList& o) const { return *p_ == *o.p_; }
    bool operator!=(const IntList& o) const { return *p_ != *o.p_; }

private:
    static const std::shared_ptr<const std::vector<int>>& emptyVec() { static const std::shared_ptr<const std::vector<int>> e = std::make_shared<const std::vector<int>>(); return e; }
    std::shared_ptr<const std::vector<int>> p_;
};

// IntSpan: an object's group list as a plain view (pointer + length) into a list the TrigShadow's load keeps (TrigShadow::groupStore_), so that copying an ObjState copies two words. IntList above is a shared_ptr: every
// copy of a timeline (one per group of every layer, some 350,000 per lv22 call) added one atomic reference count per object (8,144 of them) and dropping the timeline took them off again -- about 60 of the 157 seconds of
// that call's last plain pass (T74d, 2026-11-16). The lists never change after the load. LIFETIME: the store they point into is a shared_ptr that every TrigShadow copy holds, appended to only by addObject (a deque: its
// elements never move), so a view is valid for as long as the TrigShadow that holds it is. (A copy of an ObjState taken out of a timeline, like the live shadow's `init`, is valid as long as any TrigShadow of the level.)
class IntSpan {
public:
    IntSpan() = default;
    explicit IntSpan(const std::vector<int>& v) : p_(v.data()), n_((uint32_t)v.size()) {}
    const int* begin() const { return p_; }
    const int* end() const { return p_ + n_; }
    size_t size() const { return n_; }
    bool empty() const { return n_ == 0; }
    int operator[](size_t i) const { return p_[i]; }
    int front() const { return p_[0]; }
    int back() const { return p_[n_ - 1]; }
    const int* data() const { return p_; }
    bool operator==(const IntSpan& o) const { return n_ == o.n_ && std::equal(p_, p_ + n_, o.p_); }
    bool operator!=(const IntSpan& o) const { return !(*this == o); }
private:
    const int* p_ = nullptr;
    uint32_t n_ = 0;
};

// CowPages<T>: a vector of T held in fixed pages that copies by sharing the pages and writes by cloning the one page written (copy-on-write per page).
//
// The trigger shadow's objects (TrigShadow::objs: 8,144 of them on lv22, about 1.1 MB) were a std::vector, and every timeline of the search is a copy of its parent's: a whole-array copy for each group of
// each layer (some 350,000 a call), and the same again when the older layer is dropped -- 42 of the 157 seconds of one lv22 plain pass (2026-11-17), while one step writes a few dozen objects. Here a copy
// copies the page table (one shared_ptr a page), and a step clones only the pages it writes.
//
// Reading is `v[i]` / range-for, const only. Writing is `v.mut(i)`: it clones the page first when another copy still shares it, so a write never reaches a copy made earlier. There is no non-const operator[]
// on purpose: an assignment through `v[i]` does not compile, so every write of the shadow's objects is at a mut() the reader can find.
//   * A reference from v[i] stays VALID across a later mut() (a page is only left behind when another copy still holds it), but it is the value BEFORE that write when the mut() cloned the page: take such
//     a reference after the writes it should see, as the shadow's code does (it reads a centre object's place into locals before the loop that moves the members).
//   * The pages' counts are std::shared_ptr's (atomic): a copy may be read on other threads while its owner thread copies or writes another one. One CowPages object itself is not written from two threads.
// COWPAGES_EAGER=1 (env, read once; a check): a copy clones every page at once, which is a std::vector's copy -- the arm the lazy one is held against.
inline std::atomic<long long> g_cowPageCopies{0}, g_cowPageClones{0};   // CowPages copies made / pages cloned by a write (a report)
inline bool cowPagesEager() { static const bool e = std::getenv("COWPAGES_EAGER") != nullptr; return e; }

// Two independent 64-bit streams over the same values (128 bits together): what the trigger shadow's content hash is made of (TrigShadow::contentHash).
struct Hash2 {
    uint64_t a = 0x243f6a8885a308d3ULL, b = 0x13198a2e03707344ULL;
    static uint64_t mixA(uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); h *= 0xff51afd7ed558ccdULL; return h ^ (h >> 32); }
    static uint64_t mixB(uint64_t h, uint64_t v) { h += v * 0xc2b2ae3d27d4eb4fULL; h ^= h >> 29; h *= 0x94d049bb133111ebULL; return h ^ (h >> 31); }
    void add(uint64_t v) { a = mixA(a, v); b = mixB(b, v); }
    void addD(double d) { uint64_t u; std::memcpy(&u, &d, 8); add(u); }
    void addF(float f) { uint32_t u; std::memcpy(&u, &f, 4); add((uint64_t)u); }
    void addF2(float f, float g) { uint32_t u, w; std::memcpy(&u, &f, 4); std::memcpy(&w, &g, 4); add((uint64_t)u | ((uint64_t)w << 32)); }
    void addH(const Hash2& o) { add(o.a); add(o.b); }
    // for a set or a map: each entry is hashed on its own and the entries are summed, so the container's order does not enter
    void sumInto(uint64_t& sa, uint64_t& sb) const { sa += a; sb += b; }
    bool operator==(const Hash2& o) const { return a == o.a && b == o.b; }
    bool operator!=(const Hash2& o) const { return !(*this == o); }
};

template <class T, unsigned Shift = 5>
class CowPages {
    static constexpr size_t kN = (size_t)1 << Shift, kMask = kN - 1;
    struct Page { T v[kN]; mutable uint64_t ha = 0, hb = 0; mutable bool hok = false; };   // ha / hb: the page's content hash while hok (pagesHash; own() clears it before a write)
public:
    CowPages() = default;
    CowPages(const CowPages& o) : pages_(o.pages_), n_(o.n_) { copied(); }
    CowPages& operator=(const CowPages& o) { if (this != &o) { pages_ = o.pages_; n_ = o.n_; copied(); } return *this; }
    // operator= for a copy that holds most of o's pages already (the copy of a timeline's objects taken before each of its steps: the pages the last step wrote are the ones that differ). The pages that are
    // o's already are left as they are, so their counts are not touched: operator= counts every page of o up and every page of this down (two atomic operations a page, 0.04 ms a copy at 26,000 objects).
    void assignNear(const CowPages& o) {
        if (this == &o) return;
        if (pages_.size() != o.pages_.size() || cowPagesEager()) { *this = o; return; }
        for (size_t i = 0; i < pages_.size(); ++i) if (pages_[i] != o.pages_[i]) pages_[i] = o.pages_[i];
        n_ = o.n_;
        g_cowPageCopies.fetch_add(1, std::memory_order_relaxed);
    }
    CowPages(CowPages&&) noexcept = default;
    CowPages& operator=(CowPages&&) noexcept = default;

    size_t size() const { return n_; }
    bool empty() const { return n_ == 0; }
    const T& operator[](size_t i) const { return pages_[i >> Shift]->v[i & kMask]; }
    const T& front() const { return (*this)[0]; }
    const T& back() const { return (*this)[n_ - 1]; }
    // the element for writing: its page is cloned first when another copy shares it
    T& mut(size_t i) { return own(i >> Shift).v[i & kMask]; }
    void push_back(const T& x) {
        if ((n_ & kMask) == 0) pages_.push_back(std::make_shared<Page>());
        own(n_ >> Shift).v[n_ & kMask] = x;
        ++n_;
    }
    void assign(size_t n, const T& x) {
        pages_.clear(); n_ = 0;
        if (n == 0) return;
        auto full = std::make_shared<Page>();
        for (size_t k = 0; k < kN; ++k) full->v[k] = x;
        pages_.assign((n + kMask) >> Shift, full);   // every page starts as the one filled page: the first write to each clones it
        n_ = n;
    }
    void clear() { pages_.clear(); n_ = 0; }

    class const_iterator {
    public:
        const_iterator(const CowPages* c, size_t i) : c_(c), i_(i) {}
        const T& operator*() const { return (*c_)[i_]; }
        const T* operator->() const { return &(*c_)[i_]; }
        const_iterator& operator++() { ++i_; return *this; }
        bool operator!=(const const_iterator& o) const { return i_ != o.i_; }
        bool operator==(const const_iterator& o) const { return i_ == o.i_; }
    private:
        const CowPages* c_; size_t i_;
    };
    const_iterator begin() const { return const_iterator(this, 0); }
    const_iterator end() const { return const_iterator(this, n_); }

    // The content hash of all the pages: `f(const T* first, size_t count, Hash2& h)` hashes one page's elements; a page's hash is kept with the page until it is written again (and travels with the page
    // to every copy that shares it), so after a step only the pages that step wrote are hashed anew. The pages are folded with their index. Called by the thread that owns this object, between steps.
    template <class F>
    Hash2 pagesHash(F&& f) const {
        Hash2 r;
        for (size_t pi = 0; pi < pages_.size(); ++pi) {
            const Page& p = *pages_[pi];
            if (!p.hok) { Hash2 h; f(p.v, std::min(kN, n_ - (pi << Shift)), h); p.ha = h.a; p.hb = h.b; p.hok = true; ++hashedPages_; }
            r.a += Hash2::mixA(p.ha, (uint64_t)pi); r.b += Hash2::mixB(p.hb, (uint64_t)pi);
        }
        r.add((uint64_t)n_);
        return r;
    }
    long long hashedPages() const { return hashedPages_; }   // pages hashed anew by pagesHash since this object was made or copied (a report)

    size_t pageCount() const { return pages_.size(); }
    static constexpr size_t pageOf(size_t i) { return i >> Shift; }   // the page that holds element i

    // A watcher of some pages of the copies it is shown (`which`: page indices, the same list at every call; clear() before another list). differing() counts the listed pages that are not the ones
    // the watcher holds; sync() takes hold of those and calls f(k) for each, k = the page's place in `which`. A page the watcher holds is shared, so no copy writes it again (a write clones it first,
    // see own()) and its address is not given to another page: "the page it holds" is "the content it read from it". It reads a copy as any reader does: not while that copy is being written.
    // (A held page counts as shared in ownedPages(), and a write that would have been in place clones it: the reports of pages owned / cloned see the watcher.)
    class Watch {
    public:
        void clear() { seen_.clear(); }
        template <class Idx>
        size_t differing(const CowPages& c, const Idx& which) const {
            if (seen_.size() != which.size()) return which.size();
            size_t d = 0;
            for (size_t k = 0; k < which.size(); ++k) d += seen_[k] != c.pages_[(size_t)which[k]] ? 1 : 0;
            return d;
        }
        template <class Idx, class F>
        void sync(const CowPages& c, const Idx& which, F&& f) {
            if (seen_.size() != which.size()) seen_.assign(which.size(), nullptr);
            for (size_t k = 0; k < which.size(); ++k) {
                const std::shared_ptr<Page>& p = c.pages_[(size_t)which[k]];
                if (seen_[k] != p) { seen_[k] = p; f(k); }
            }
        }
    private:
        std::vector<std::shared_ptr<Page>> seen_;
    };
    // pages this copy shares with no other (a report: what a step has written since the copy was made, while the copy's parent is alive)
    size_t ownedPages() const { size_t k = 0; for (const auto& p : pages_) if (p.use_count() == 1) ++k; return k; }

private:
    Page& own(size_t pi) {
        std::shared_ptr<Page>& p = pages_[pi];
        if (p.use_count() > 1) { p = std::make_shared<Page>(*p); g_cowPageClones.fetch_add(1, std::memory_order_relaxed); }
        p->hok = false;   // about to be written
        return *p;
    }
    void copied() {
        g_cowPageCopies.fetch_add(1, std::memory_order_relaxed);
        if (cowPagesEager()) for (auto& p : pages_) p = std::make_shared<Page>(*p);
    }
    std::vector<std::shared_ptr<Page>> pages_;
    size_t n_ = 0;
    mutable long long hashedPages_ = 0;
};

}  // namespace gdtrig
}  // namespace dp
