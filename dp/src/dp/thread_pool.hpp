#pragma once
#include "dp/state.hpp"

namespace dp {

// A fixed pool with a generation counter. Threads are created ONCE: a layer is
// ~microseconds of work per state and a level is ~20,000 layers, so spawning
// per layer would cost more than it saves.
class ThreadPool {
public:
    explicit ThreadPool(int n) {
        // Each worker counts into its own row of the per-thread tallies
        // (g_threadSlot); the thread that owns the pool keeps row 0.
        for (int i = 0; i < n; ++i)
            ths_.emplace_back([this, i] { g_threadSlot = i + 1; worker(); });
    }
    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
            ++gen_;
            genSpin_.store(gen_, std::memory_order_release);
        }
        cvStart_.notify_all();
        for (auto& t : ths_) t.join();
    }
    size_t size() const { return ths_.size(); }
    // fn(i) for i in [0,count). Each i writes its OWN slot, so the result does
    // not depend on which thread ran which chunk -- the output stays
    // bit-identical to the serial run, which is what the whole project rests on.
    void parallelFor(size_t count, const std::function<void(size_t)>& fn) {
        dispatch(count, fn, chunkFor(count));
    }
    // One task per index. parallelFor's 32-wide chunking is right for the
    // per-child stepping, but with a handful of COARSE tasks (the dedupe
    // shards) it would hand all of them to whichever thread got there first
    // and the "parallel" phase would run serial.
    void parallelTasks(size_t count, const std::function<void(size_t)>& fn) {
        dispatch(count, fn, 1);
    }

private:
    // A short job is cut into about two chunks a thread (down to kChunkMin children each), so that a small group's
    // step reaches the whole pool instead of two threads or one. Which thread runs which chunk changes nothing in the
    // result (every index writes its own slot), only how long the job takes: on a level whose frontier splits into
    // five groups a layer, half of them under 128 parents, the step blocks went from 57% busy to 75%.
    size_t chunkFor(size_t count) const {
        const size_t per = count / ((ths_.size() + 1) * 2);
        return std::max(kChunkMin, std::min(kChunk, per));
    }
    void dispatch(size_t count, const std::function<void(size_t)>& fn,
                  size_t chunk) {
        // A job no bigger than one chunk per thread is not worth waking anyone
        // for: the wake-up and the wait cost more than the work. Profiled on a
        // level with 868 moving objects, the workers sat idle ~70% of the run
        // while the frontier was split into small groups, each dispatched on its own. Every
        // index still runs exactly once and writes only its own slot, so the
        // result is the same whichever thread runs it.
        if (ths_.empty() || count == 0 || (chunk > 1 && count <= chunk)) {
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
            doneSpin_.store(0, std::memory_order_relaxed);
            ++gen_;
            genSpin_.store(gen_, std::memory_order_release);
        }
        cvStart_.notify_all();
        runChunks();          // the caller is a worker too
        {   // the caller has nothing else to do: wait for the others by spinning first, a kernel wake-up is most of a short job
            const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(kCallerSpinUs);
            while (doneSpin_.load(std::memory_order_acquire) != ths_.size() && std::chrono::steady_clock::now() < until)
                std::this_thread::yield();
        }
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
            // Spin briefly before sleeping: the next group's job usually comes
            // within microseconds, and a kernel wake-up per group was most of
            // what the idle time was made of. Purely a matter of who waits how.
            {
                const auto until = std::chrono::steady_clock::now()
                                   + std::chrono::microseconds(50);
                while (genSpin_.load(std::memory_order_acquire) == seen
                       && std::chrono::steady_clock::now() < until)
                    std::this_thread::yield();
            }
            std::unique_lock<std::mutex> lk(m_);
            cvStart_.wait(lk, [this, &seen] { return gen_ != seen; });
            seen = gen_;
            if (stop_) return;
            lk.unlock();
            runChunks();
            lk.lock();
            doneSpin_.store(done_ + 1, std::memory_order_release);
            if (++done_ == ths_.size()) {
                lk.unlock();
                cvDone_.notify_one();
            }
        }
    }
    static constexpr size_t kChunk = 32;
    static constexpr size_t kChunkMin = 8;
    static constexpr int kCallerSpinUs = 200;
    size_t chunk_ = kChunk;
    std::vector<std::thread> ths_;
    std::mutex m_;
    std::condition_variable cvStart_, cvDone_;
    const std::function<void(size_t)>* fn_ = nullptr;
    size_t count_ = 0;
    std::atomic<size_t> next_{0};
    size_t gen_ = 0;
    std::atomic<size_t> genSpin_{0};   // gen_, readable without the lock (spin only)
    std::atomic<size_t> doneSpin_{0};  // done_, readable without the lock (the caller's spin only)
    size_t done_ = 0;
    bool stop_ = false;
};

inline size_t g_aliveCap = 16000;  // per-layer stride cap (keeps RAM sane)
// --capmap x0:c0,x1:c1,...: the alive cap as a step function of the frontier's
// leading x (the previous layer's, which is what exists when the cap is applied).
// From x_i on the cap is c_i; before the first entry it is --cap. Empty = --cap
// everywhere, i.e. the search without the flag. A search restriction, not physics:
// every plan it can emit is one the model steps exactly as before.
inline std::vector<std::pair<double, size_t>> g_capMap;
inline size_t capAtX(double x) {
    size_t c = g_aliveCap;
    for (const auto& e : g_capMap)
        if (x >= e.first) c = e.second;
    return c;
}
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
// THE GLITCH-AVOID MODE (both 0 = off; the user's option, never a cold's default). Routes that
// only work by frame-level precision are dropped from the search -- not the physics, which stays
// GD's. Two shapes, each measured on an official solution the user named as a glitch:
//   --glitchportal <px>  a branch that passes a portal which would change something (mode,
//                        size, gravity, dual, teleport AND speed) by less than this is dropped,
//                        unless it is still closing on it -- g_portalDodgeMin / g_speedDodgeMin
//                        widened. lv17's wave slips under a ship portal 8-10 px clear, lv22's
//                        early dash release passes a 1x portal 12.3 px clear.
//   --glitchwave <px>    a wave that comes this close to something that kills it (a hazard, or a
//                        solid it cannot slide on) is dropped. lv17's wave rides 0-4 px over a
//                        row of spikes, switching the button every 2 ticks. The level's floor and
//                        ceiling are not objects and the 1755 slide seat is exempt: touching those
//                        is how the wave is meant to be flown.
//   --glitchembed <px>   a ship, ufo or swing that ends a tick with its outer box this deep in a
//                        solid is dropped: the ship half sunk into a step, which lives only
//                        because GD's crush test reads the inner box. The depth has to stay above
//                        what ordinary flying reaches: lv1's third coin sits behind a slot exactly
//                        as tall as the ship, flown 11.4 px into its step, and the mod's default
//                        is 14 (config.hpp kGlitchEmbedDefault).
inline double g_glitchPortal = 0.0;
inline double g_glitchWave = 0.0;
inline double g_glitchEmbed = 0.0;
//   --wavecentre <px>   the wave-behaviour option (Avoid glitches, third choice; 0 = off): a wave that ends
//                       a tick within this many px of the band's floor or ceiling, of a solid (the faces it
//                       could slide on included, unlike --glitchwave), or riding a ramp is dropped -- it is
//                       crawling along the terrain, not flying the corridor. Only the clearance is asked
//                       for, so a corridor narrower than twice this leaves no wave states (the loop then
//                       falls back to --glitchwave alone, src/mod/repair.hpp escalate).
inline double g_waveCentre = 0.0;
//   --wavetidy <S> / --wavetoggle <T>   a PREFERENCE among wave routes, not a drop (0 = off). The route's own
//                       State::tight (the goal pick already reads it: lower is roomier) takes, on every wave tick,
//                       floor(S * off^2) where off is how far the wave sits from the middle of the corridor it is
//                       in (0 centre, 1 wall; corridor = the band's floor and ceiling and the solids above and
//                       below), and T for every change of the button. With either on, each layer's frontier is
//                       put in stable order of that score before it is stepped, so the first-accepted-wins dedupe
//                       keeps the tidier route into a cell.
inline double g_waveTidy = 0.0;
inline int g_waveToggle = 0;
//   --startedge <ticks>  an anchored call: the ticks the button had already been in its present state at the anchor
//                        (State::edgeAge of the start state; -1 = not said, which leaves it at "long ago"). The mod
//                        reads it off the plan the game flew, so --wavepulse / --minpulse hold across a re-anchor.
inline int g_startEdge = -1;
//   --glitchdeco        a decoration that is a hazard's look (90% of its placements sit on
//                        that hazard) and is placed without one is loaded as that hazard
//                        (level_loader.hpp addHazardLookalikes, where g_glitchDeco lives: the
//                        loader comes first in the header chain): the hole in lv20's first
//                        spike wall is two such edge pieces with nothing under them.
// --coinwin <yBin>,<vyBin>,<len> (all 0 = off): with --coins, a state inside the approach window
// of a coin it still lacks -- x in [coin - len, coin + hw], frame 0, the coin's miss prune final --
// gets a cap class of its own per (y / yBin, vy / vyBin) cell, so the alive cap's water-fill keeps
// a share of every cell there instead of one stride over the whole layer. lv22's second coin sits
// in a pocket above a spike row, entered from below through a gap: the route dives to the floor
// ~520 px before the coin and climbs at full speed from there, a minority of the swing layer that
// the stride thins out long before the gap. At cap 40,000 every branch then passes under the coin
// and the miss prune empties the frontier (x=10,555, from every anchor the ladder tries -- in the
// ordinary coin run too, where only the in-game section solve found the pocket). With the cells the
// same call keeps it and takes the coin. The cell needs BOTH axes: a y grid alone is still pruned
// (the pocket route is not rare in y, only in y and vy together).
inline double g_coinWinY = 0.0;
inline double g_coinWinVy = 0.0;
inline double g_coinWinLen = 0.0;
// --ladderback (on by default since 2026-10, --no-ladderback the off arm; cli.hpp cliMain, step
// 3; it acts only inside --capladder): a death inside a --capladder window changes one
// thing at a time -- the lead (x2) or the cap (x4) -- starting with the lead, keeping a kind that
// moved the death (a later death tick) and swapping one that did not. A wall that one step of each
// kind has left in place, or with both kinds spent, goes to the plain search as the default's
// does. The default raises both together, so a window has the full cap after two repeats at a lead
// of 1,200 px and the ladder goes to the plain search, which drops the grid and searches the whole
// level at the full cap. Walls come in kinds: lv14's crossed with a longer lead at cap 500 where
// the default had gone to 2000 and then plain; lv19's at x 28,074 did not move for any lead at cap
// 500 and moved at once at cap 2000; lv20's at x 22,888 moves for nothing at all.
// Official 1-22 from t=0 on RC9, the loop's base search, offline: 153.5 -> 136.7 s coins off,
// 160.1 -> 163.3 s coins on, 22/22 either way (lv14 24 -> 9 s; lv19 20 -> 23 s and lv21 10 ->
// 11 s pay for the lead step at a wall that wanted the cap).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// (--ladderx0, a measurement arm that ran the window from x 0 at the full cap once before the plain search at
// a wall neither kind of step moved, is gone: it crossed one wall (34 -> 11 s) and cost every wall no search
// crosses a second plain-sized search, e.g. 251 -> 853 s of dp on a coin run.)
// --ladderovercap: a window that neither kind of step moves, with its cap at --cap,
// gets up to two more attempts with the WINDOW's cap at 2x and then 4x --cap before the plain search
// (the plain search is --cap everywhere, the same ceiling, so it cannot cross what the window could
// not). The rest of the map keeps its caps, so the extra breadth is paid only over the window, and
// the memory gate (--memlimit) still bounds it. lv22's anchor at t=6,986 (the swing portal at
// x 9,315): every window at cap 2000 and the plain search die in the climb at t=7,877-8,134, while
// the same window at 4000 or 8000 reaches t=11,948 -- the window's position was never the limit.
// On by default since 2026-10-07 (lv22 cold without a section-solver handoff); --no-ladderovercap is the
// off arm.
inline bool g_ladderOverCap = true;
// --ladderovercapstop (off): the second over-cap step (the window at 4x --cap) is skipped for the plain search when the
// first (2x) left the death where it was: not later than the attempt before it, at the same state count to the
// state. A long ship/wave level: in 17 whole-level ladder calls the 8000 rung found exactly what the 4000 one had,
// 53 s of the run's 1,715. Where the 2x step moves the death the 4x step is taken as before.
inline bool g_ladderOverCapStop = false;
// --capenvelope (off; --ladderenv passes it to the attempts after a coin's miss prune, cli.hpp
// cliMain): a class the alive cap thins keeps its lowest and its highest state
// (by y, then vy, then layer order) before the stride takes the rest of its share. The stride
// samples in layer order, so a lane at the edge of a class's heights is a minority it can drop on
// any layer, and a lane dropped once is gone (lv1 with coins, cap 125: the high lane to the last
// coin drifts out over ~2,300 px). On every search it costs more than it saves: official 1-22 from
// t=0, 184 -> 238 s coins off, lv11 6 -> 34 s.
inline bool g_capEnvelope = false;
// --ladderenv (on by default since 2026-10, --no-ladderenv the off arm; with --ladderback): the
// envelope step above. A frontier that died at a coin's
// miss prune (SearchOutcome::coinWall: the deepest x at the far edge of a coin whose passing is
// final) gets the next attempt with --capenvelope at once, once per coin; kept for the rest of the
// ladder if it moved the death, dropped if not. Nowhere else: without coins the ladder is
// --ladderback's own. Official 1-22 from t=0 on RC9, with coins: 163.3 -> about 146 s, lv1 26 ->
// 4 s.
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// (--rotport lives in bands.hpp: dynamics.hpp reads it and comes first in the
// header chain.)
// out-of-play bound, set from the level's own geometry (see Level::maxY)
inline double g_yBound = 700.0;
// ...and the same for a TURNED frame, where `y` is a world X (see RotTrig).
// Set from the level's x extent at load; only consulted when frame != 0.
inline double g_yBoundTurned = 1e9;

}  // namespace dp
