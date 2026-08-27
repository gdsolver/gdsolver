#pragma once
#include "dp/state.hpp"

namespace dp {

// A fixed pool with a generation counter. Threads are created ONCE: a layer is
// ~microseconds of work per state and a level is ~20,000 layers, so spawning
// per layer would cost more than it saves.
class ThreadPool {
public:
    explicit ThreadPool(int n) {
        for (int i = 0; i < n; ++i) ths_.emplace_back([this] { worker(); });
    }
    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
            ++gen_;
        }
        cvStart_.notify_all();
        for (auto& t : ths_) t.join();
    }
    size_t size() const { return ths_.size(); }
    // fn(i) for i in [0,count). Each i writes its OWN slot, so the result does
    // not depend on which thread ran which chunk -- the output stays
    // bit-identical to the serial run, which is what the whole project rests on.
    void parallelFor(size_t count, const std::function<void(size_t)>& fn) {
        dispatch(count, fn, kChunk);
    }
    // One task per index. parallelFor's 32-wide chunking is right for the
    // per-child stepping, but with a handful of COARSE tasks (the dedupe
    // shards) it would hand all of them to whichever thread got there first
    // and the "parallel" phase would run serial.
    void parallelTasks(size_t count, const std::function<void(size_t)>& fn) {
        dispatch(count, fn, 1);
    }

private:
    void dispatch(size_t count, const std::function<void(size_t)>& fn,
                  size_t chunk) {
        if (ths_.empty() || count == 0) {
            for (size_t i = 0; i < count; ++i) fn(i);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            fn_ = &fn;
            count_ = count;
            chunk_ = chunk;
            next_.store(0, std::memory_order_relaxed);
            done_ = 0;
            ++gen_;
        }
        cvStart_.notify_all();
        runChunks();          // the caller is a worker too
        std::unique_lock<std::mutex> lk(m_);
        cvDone_.wait(lk, [this] { return done_ == ths_.size(); });
        fn_ = nullptr;
    }
    void runChunks() {
        for (;;) {
            size_t i = next_.fetch_add(chunk_, std::memory_order_relaxed);
            if (i >= count_) return;
            const size_t e = std::min(i + chunk_, count_);
            for (; i < e; ++i) (*fn_)(i);
        }
    }
    void worker() {
        size_t seen = 0;
        for (;;) {
            std::unique_lock<std::mutex> lk(m_);
            cvStart_.wait(lk, [this, &seen] { return gen_ != seen; });
            seen = gen_;
            if (stop_) return;
            lk.unlock();
            runChunks();
            lk.lock();
            if (++done_ == ths_.size()) {
                lk.unlock();
                cvDone_.notify_one();
            }
        }
    }
    static constexpr size_t kChunk = 32;
    size_t chunk_ = kChunk;
    std::vector<std::thread> ths_;
    std::mutex m_;
    std::condition_variable cvStart_, cvDone_;
    const std::function<void(size_t)>* fn_ = nullptr;
    size_t count_ = 0;
    std::atomic<size_t> next_{0};
    size_t gen_ = 0;
    size_t done_ = 0;
    bool stop_ = false;
};

inline size_t g_aliveCap = 16000;  // per-layer stride cap (keeps RAM sane)
// --memstat: extended per-report diagnostics. Off by default -- walking the
// arena is O(arena) per report and only the memory work needs the numbers.
inline bool g_memStat = false;
// --gcnodes: mark-compact the witness arena when it doubles past this many
// nodes (see the GC block in the search loop). 0 = never compact.
inline size_t g_gcNodes = 8000000;
// --memlimit: hard budget in MiB for the search's own structures; on breach
// the run emits an alive prefix and says MEMORY_LIMIT instead of letting the
// OS kill it as exit 255. 0 = no budget.
inline size_t g_memLimitMiB = 6144;
// --dodgemin: how much clear air a branch needs to claim it slipped PAST a
// portal instead of through it. Below this the portal is treated as
// unavoidable and the dodging branch is dropped (see the portal loop).
// 0.1 px. The lv18 dodge this exists for is 0.008 px -- the level's portal is
// at cy=266.992 rather than 267, so its top edge lands 0.008 px under the
// flying band's ceiling, and the plan rides that ceiling. A tenth of a pixel is
// an order of magnitude clear of that and still nowhere near a designed gap.
// TRIED: 1.0 px. Too wide -- lv16 went from CLEARED in 13 iterations to
// oscillating (17,822 -> 8,090 -> 12,605 at iteration 25), so real routes there
// do pass portals inside a pixel. 0 restores the old behaviour.
inline double g_portalDodgeMin = 0.1;
// ...and the same for SPEED portals, but OFF by default (see the note at the
// use site). --speeddodge <px>.
inline double g_speedDodgeMin = 0.0;
// (--rotport lives in bands.hpp: dynamics.hpp reads it and comes first in the
// header chain.)
// out-of-play bound, set from the level's own geometry (see Level::maxY)
inline double g_yBound = 700.0;
// ...and the same for a TURNED frame, where `y` is a world X (see RotTrig).
// Set from the level's x extent at load; only consulted when frame != 0.
inline double g_yBoundTurned = 1e9;

}  // namespace dp
