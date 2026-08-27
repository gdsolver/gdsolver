#pragma once
#include "dp/fixup.hpp"

namespace dp {

// ---- state that must not survive one call into the next -------------------
//
// The solver was written as a process: `leveldp` parses a command line, loads a level, searches,
// prints, exits. Under that assumption a namespace-scope global IS per-solve state, and nothing
// ever had to clear one.
//
// The mod broke that assumption. It calls cliMain in-process (src/mod/dp_bridge.cpp) dozens of
// times per level, so every one of those globals now reaches the NEXT call -- and only ever the
// mod's next call, never the CLI's. That is the worst shape a bug can have: it cannot be
// reproduced with the tool everything is measured with.
//
// It is not theoretical. Measured with dp/src/seqcall_main.cpp on lv16, running the SAME
// arguments twice -- once alone, once after an unrelated call that had a start band, needtrig
// flags, a spent rotation and a fixups file:
//
//   alone        maxAlive=3528 capHits=1772 dropped=974313, plan 1,416 bytes
//   after it     maxAlive=3540 capHits=1775 dropped=972541, plan 1,444 bytes
//
// Different plan, different trace, from identical arguments. Some of the leaks are worse than
// drift: `g_needSkip` only ever ORs bits in, so a box skipped once is skipped for the rest of
// the session; `g_startBandSet` never goes back to false, so a call with no band inherits the
// previous one's; `g_touch` is appended to by every load, so a level's touch triggers pile up in
// a list that is read only 32 entries deep; and `g_baseLv` points at a Level local to cliMain,
// which is dangling the moment it returns.
//
// So: reset to exactly what a freshly started process would hold. For the CLI this is a no-op --
// it runs once -- which is also how the change is proven safe: byte-identical solver output on
// the replay/cold suite (py/quick_regress.py; see rule 3 in CLAUDE.md).
//
// TWO DELIBERATE EXCEPTIONS, both set from OUTSIDE a call:
//   * g_levelCsv -- the mod puts the level in here before calling and clears it after, so this
//     is the caller's data, not the last solve's. Clearing it here would leave the mod unable to
//     load a level at all.
//   * g_progress -- a live readout the mod polls from another thread while the search runs. It
//     belongs to the call in flight, not to the one that finished; cliMain re-opens it at the
//     search. (g_outcome IS reset, by cliMain itself, and for the opposite reason: a stale
//     verdict must never be readable as this call's answer.)
//
// WHEN ADDING A GLOBAL TO dp/, ADD IT HERE. The guard against forgetting is seqcall: run the
// same arguments alone and after a loaded call and diff the plan.
inline void resetInvocationState() {
    // bands.hpp
    g_startBandSet = false;
    g_startBandFloor = 0.0;
    g_startBandCeil = 1e9;
    g_bandTrack.clear();
    g_bandTrackCam = -1;
    g_bandK = 0.0;
    g_slopeDbg = false;
    g_bandDbg = false;
    g_shipCeilSet = false;
    g_playerCeils.clear();

    // constants.hpp -- the measured values, which --flags override per call
    g_noMiniWave = false;
    g_oldLatency = false;
    g_oldSlope = false;
    kLandTol = 10.0;
    kStickGap = 4.034;
    kRotPerpWin = 150.0;
    g_noHangLadder = false;
    g_noCrush = false;
    kCrushHalf = 4.5;
    kStepDepth = 3.15;
    g_ceilPin = false;
    g_shipCeil = 375.0;
    g_flyFloor = 0.0;
    g_ufoCeil = 0.0;

    // dynamics.hpp
    g_dynInterp = true;
    g_recPhase = 0;
    g_autoTrig.clear();
    g_rotated.clear();
    g_trigClosed = true;
    g_trigRaw = false;
    g_dynDbg = -1;

    // fixup.hpp -- --fixups appends, so without this the file is loaded once per call and the
    // records of every earlier call are still in the list
    g_fixups.clear();
    g_fixupKills.clear();
    g_fixupDeltas.clear();
    g_fixupHits = 0;

    // frames.hpp
    g_rotTrig.clear();
    g_spentRot.clear();
    g_revToggle = true;
    g_ctrlWin.clear();
    g_winRePushJump.clear();

    // level.hpp -- the rotated copies belong to the level that built them, and g_baseLv points
    // into cliMain's own frame
    for (auto& f : g_frameLv) f.reset();
    g_baseLv = nullptr;

    // level_loader.hpp (g_levelCsv is the caller's -- see above)
    g_obb.clear();

    // modifiers.hpp
    g_deadBands.clear();
    g_forceBoxes.clear();
    g_forceFields.clear();
    g_timeWarps.clear();
    g_zoomTrigs.clear();
    g_flipHeadBoxes.clear();
    g_dashStopBoxes.clear();

    // search_key.hpp
    g_topAhead.clear();
    g_bucketX0 = 0.0;
    g_shipYq = 0.5;
    g_shipVq = 2.5;
    g_cubeYq = 2.0;
    g_cubeVq = 10.0;
    g_dualFreeQ = 0.125;

    // speed.hpp
    kSupportTol = 0.01;
    g_startSpeedMul = 0.0;
    g_dynHazPad = 0.0;
    g_maxPlayY = 1e18;
    g_shiftDbgUid = -1;
    g_shiftDbgDone = false;

    // stairs.hpp -- a stream owned by the call that opened it
    g_snapOut = nullptr;

    // state.hpp
    g_threads = 4;

    // thread_pool.hpp
    g_aliveCap = 16000;
    g_memStat = false;
    g_gcNodes = 8000000;
    g_memLimitMiB = 6144;
    g_portalDodgeMin = 0.1;
    g_speedDodgeMin = 0.0;
    g_rotPort = false;
    g_rotLast = false;
    g_yBound = 700.0;
    g_yBoundTurned = 1e9;

    // triggers.hpp
    g_touch.clear();
    for (auto& f : g_touchFrame) f.clear();
    for (int b = 0; b < 32; ++b) g_touchFireT[b] = -1;
    g_trigReported = 0;
    g_bandPath.clear();
    g_bands.clear();
    g_needTrig = 0;
    g_needUnseen = false;
    g_needSkip = 0;
    g_oriented = true;
    g_obbAll = false;
    g_invCubeFloorKill = false;
}

}  // namespace dp
