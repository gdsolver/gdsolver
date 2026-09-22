#pragma once
#include "dp/fixup.hpp"
#include "dp/clearance.hpp"
// ...and the reference watch, whose globals this file clears. It was not in the
// chain: only cli.hpp includes this header, and cli.hpp had already pulled
// refwatch.hpp in, so the omission was invisible until src/mod/dp_bridge.cpp --
// the other caller of resetInvocationState -- failed to compile. refwatch.hpp
// includes frames.hpp and state.hpp and nothing includes reset.hpp except
// cli.hpp, so there is no cycle.
#include "dp/refwatch.hpp"

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
// the session; the start band's kind never went back to "none given", so a call with no band
// inherited the previous one's; `g_touch` is appended to by every load, so a level's touch pile up in
// a list that is read only 32 entries deep; and `g_baseLv` points at a Level local to cliMain,
// which is dangling the moment it returns.
//
// So: reset to exactly what a freshly started process would hold. For the CLI this is a no-op --
// it runs once -- which is also how the change is proven safe: byte-identical solver output on
// the replay/cold suite (py/quick_regress.py; see rule 3 in CLAUDE.md).
//
// **Write the DECLARED default, not the value it happens to have today.** A line here that
// repeats a literal is a second copy of the default, and when the declaration is changed and
// this is not, every in-process solve silently runs the OLD behaviour while the header says
// otherwise -- which is un-reproducible with the CLI, exactly as above. That cost a day on
// 2026-08-28: a rule flipped on in step.hpp was held off by a `= false` here, and the level it
// was measured on came back failing with the rule's own signature. Prefer naming the constant
// the declaration uses; if the default is a bare literal, changing it means changing both.
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
    g_startBandKind = BandKind::Unknown;
    g_startBandFloor = 0.0;
    g_startBandCeil = 0.0;
    g_bandTrack.clear();
    g_bandTrackIntervals.clear();
    g_bandTrackCam = -1;
    g_bandTrackEnd = -1;
    g_bandK = 0.0;
    g_slopeDbg = false;
    g_slopeRelDbg = false;
    g_fxWatchT = -1;
    g_fxWhy[0] = 0;
    g_vyWatchT = -1;
    g_vyWriter = 0;
    g_vyWrites = 0;
    g_yWriter = 0;
    g_yWrites = 0;
    g_yIn = 0.f;
    g_vyIn = 0.f;
    g_seatGateSeen = 0;
    g_seatImpulsedOff = g_seatTappedOff = g_seatOnSlope = g_seatTook = -1;
    g_impulseSite = 0;
    g_vpWrites = 0;
    g_impCount = 0;
    g_vyWatchT2 = -1;
    g_dcyDbg = false;
    g_bandDbg = false;
    g_spdDbg = false;
    g_shipCeilSet = false;
    g_playerCeils.clear();

    // clearance.hpp -- instrumentation. The tallies are per solve, so a second
    // in-process call must not read the first one's cells.
    g_clearProbe = false;
    g_clear.reset();
    g_goalDiv = GoalDiversity{};

    // constants.hpp -- the measured values, which --flags override per call
    g_swingPushTol = false;
    g_hazDbgUid = -1;
    g_hazAabb = false;
    g_inRecon = false;
    g_dieRecon = 0;
    g_dieReconWhy.clear();
    g_resimDead = 0;
    g_resimTicks = 0;
    g_resimFirst = -1;
    g_resimLast = -1;
    g_resimWhy = nullptr;
    g_trigDbgT = -1;
    g_gfireGroups = 0;
    g_gfireSpread = 0;
    g_gfireSum = 0;
    g_gfireMax = 0;
    g_resimUid = -1;
    g_resimObjX = 0.f;
    g_resimObjY = 0.f;
    g_resimTrig = 0;
    g_resimFrame = -1;
    g_halfNow = 0;
    g_noSlopeSeat = false;
    g_ceilRideSlope = false;
    g_lawSeatOnSlope = false;
    g_pinMinNoSwing = false;
    g_groupHoldEnd = 0;
    g_groupHoldDeath = false;
    g_noPadPlayerRot = false;
    g_noSatRotRaw = false;
    g_oldLatency = false;
    g_oldSlope = false;
    kLandTol = 10.0;
    kStickGap = 4.034;
    kRotPerpWin = 150.0;
    kCrushHalf = 4.5;
    kStepDepth = 3.15;
    g_ceilPin = false;
    // The mod calls the solver more than once per process, so a flag left set by
    // the previous level would silently put coin routing on for the next one
    // (the in-process caller's globals are this file's whole subject).
    g_coinRoute = false;
    g_coinMaskSeed = 0;
    g_coinSkip = 0;
    g_itemBase.clear();
    g_collect.clear();
    kCoinMargin = 0.0;
    g_shipCeil = 375.0;
    g_flyFloor = 0.0;
    g_ufoCeil = 0.0;

    // dynamics.hpp
    g_recPhase = 0;
    g_autoTrig.clear();
    g_rotated.clear();
    g_rotSpec.clear();
    g_trigRaw = false;
    g_lagFitDbg = false;
    g_touchEnteredGiven = false;
    g_touchEntered.clear();
    g_touchEnteredT.clear();
    g_dynDbg = -1;
    g_formulaDriven = 0;
    // ...and three flags the audit had been carrying as known defects. Two are
    // diagnostics, but g_rotSplit is not: it is declared TRUE and --no-rotsplit
    // (cli.hpp:152) turns it off with nothing to turn it back on, so ONE call
    // passing that flag disables rotation-orbit splitting for every later solve
    // in the process -- and rotation splitting is physics, not instrumentation
    // (it is what lv21's 55 px y error turned out to be).
    g_rotCheck = false;
    g_rotSplit = true;
    g_shiftStat = false;
    // ...and the locked box, which the loader finds. Same trap as
    // g_touchMoveTicks: left behind, the next level in a one-session run reads
    // the PREVIOUS level's box index, and a one-session run is exactly the
    // arrangement the serial cold uses.
    g_lockBox = -1;
    g_lockTicks = 0.0;

    // fixup.hpp -- --fixups appends, so without this the file is loaded once per call and the
    // records of every earlier call are still in the list
    g_fixupConflictFilter = false;
    g_fixups.clear();
    g_fixupKills.clear();
    g_fixupDeltas.clear();
    g_fixupHits = 0;
    for (int i = 0; i < 4; ++i) g_fixupHitFrame[i] = g_fixupCallFrame[i] = 0;
    g_fixupRotSeen = 0;
    g_fixupCallRotSeen = 0;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 2; ++j)
            g_frameRevReach[i][j] = g_frameRevCall[i][j] = g_frameRevHit[i][j] = 0;

    // frames.hpp
    g_rotTrig.clear();
    // ...and the queue built from them. Same pairing rule as everywhere else in
    // this file: it is constructed per level, so it is cleared per call.
    g_rotQ.clear();
    g_rotQBeg.fill(0);
    g_rotQEnd.fill(0);
    g_rotQChanMask.fill(0);
    g_rotQChans = 0;
    g_rotQueue = false;
    g_rotWatchLo = -1;
    g_rotWatchHi = -1;
    g_qfoldLo = -1;
    g_qfoldHi = -1;
    g_rotQToggle = false;
    g_rotQOff.clear();
    g_rotQOn.clear();
    g_rotTrigOff.clear();
    g_rotTrigOn.clear();
    g_touchSeedArg.clear();
    g_touchCensus = false;
    g_tcBranch = 0;
    g_tcBranchP1 = 0;
    g_touchStops.clear();
    g_ceilPush = false;
    g_latGap = false;
    g_bonkArm = false;
    g_vetoPhys = false;
    g_verdictInfo = false;
    g_resimPX = 0.f;
    g_preBtnSet = false;
    g_preBtnY = 0.0;
    g_startRotChan = -1;
    g_startRotRev = 0;
    g_startRotSpent.clear();
    g_seedDump = -1;
    g_seedEvery = 0;
    g_p2Touch = false;
    // ...and the anchor payload. Left behind, LEVEL N's payload seeds LEVEL
    // N+1 in a one-session cold run -- and it does so plausibly, planning
    // against a world where triggers are already open, which is the shape
    // CLAUDE.md records as having hidden under a green 22/22 for months. A
    // launch per level cannot reach it, so no per-level regression will.
    g_anchorState.clear();
    // ...and the claims that payload made. `owns=touch` / `owns=portal` are only
    // ever set to true, by parsing an anchor payload (cli.hpp:1148-1150), and
    // nothing sets them back -- so in the mod a level whose payload claimed a
    // subsystem hands that claim to every later call in the session, including
    // calls whose payload claims nothing. It is not inert: cli.hpp:1239 branches
    // on g_ownsPortal, and 1174-1180 turn a claim into a hard refusal when the
    // key it implies is absent, so an inherited claim can refuse a payload that
    // is complete for what it actually declares. config.hpp:212-220 already
    // plans to turn portalPayload on for cold verdicts, which is the gate that
    // makes this reachable rather than merely latent.
    g_ownsTouch = false;
    g_ownsPortal = false;
    g_ownsHist = false;
    g_seedPartialOk = false;
    g_seedPartial.clear();
    g_spentRot.clear();
    g_spentPad.clear();
    g_spentPadSeed = true;
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
    g_forceIdsPath.clear();
    g_forceUnit1x = false;
    g_forceFields.clear();
    g_timeWarps.clear();
    g_zoomTrigs.clear();
    // bands.hpp: the objrects header's own answer to "does this dump carry the
    // Free Mode / touch / spawn columns". Per level, so it has to be cleared
    // with everything else -- a stale `true` would let the next level in the
    // same process use the band's own height with no gate behind it.
    g_freeModeCol = false;
    g_trigGateCol = false;
    g_staticCamCol = false;
    g_staticCams.clear();
    g_flipHeadBoxes.clear();
    g_dashStopBoxes.clear();
    g_armBoxes.clear();
    g_slideBoxes.clear();

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
    g_fixRadiusCollision = 0;
    g_fixGravityBug = 0;
    g_fixNegativeScale = 0;
    g_fixRobotJump = 0;
    g_dynamicLevelHeight = 0;
    g_maxPlayY = 1e18;
    g_offBoardMargin = 0.0;
    g_shiftDbgUid = -1;
    g_shiftDbgDone = false;

    // stairs.hpp -- a stream owned by the call that opened it
    g_snapOut = nullptr;

    // state.hpp
    g_threads = 4;

    // thread_pool.hpp
    g_aliveCap = 16000;
    g_memStat = false;
    g_phaseProf = false;
    // progress.hpp's checkpoint channel is NOT reset here: its `enabled`/`cancel` belong to the
    // CALLER -- the mod clears `cancel` before every solve and turns `enabled` on for the session,
    // and a per-invocation reset would silently undo both.
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
    // ...and the per-box move length derived from it. Built beside g_touch in
    // cliMain, so it is cleared beside g_touch here: left behind, the next
    // level in a one-session run keys its states against the PREVIOUS level's
    // box durations, and a leak of exactly that shape lived under a green
    // 22/22 for months.
    g_touchMoveTicks.clear();
    for (auto& f : g_touchFrame) f.clear();
    // kTouchBits, not 32: the array is std::array<int, kTouchBits>, so a cap of
    // 32 left slots 32.. holding the previous level's fire ticks in a
    // one-session run -- the leak this block exists to close.
    for (int b = 0; b < kTouchBits; ++b) g_touchFireT[b] = -1;
    // ...and the boxes' travel coordinates, the last of the touch family that
    // was not here. A level with 32 boxes followed by one with 5 leaves the
    // previous level's coordinates in slots 5..31, where the 400 px proximity
    // gates read them. No firing has been shown today -- the readers index by
    // State::trig bits and the loader only assigns bits below out.size() -- but
    // the anchor path re-windows that set (triggers.hpp:437-448) and this
    // session is changing how anchored bits are made, so the tail stops being
    // unreachable by construction.
    for (int b = 0; b < kTouchBits; ++b) g_touchBoxU[b] = 0.f;
    // ...and the key census tally, for the same reason as everything else here:
    // the mod calls cliMain many times per level, and a per-solve count that
    // carried would be a sum over calls wearing one call's name.
    g_keyCensus = false;
    for (int b = 0; b < kTouchBits; ++b) g_keyCount[b].store(0);
    // ...and the per-coin closest approach, for the same reason: a second solve
    // in one process would otherwise report the first one's nearest miss.
    for (int i = 0; i < 8; ++i) {
        g_coinNearDy[i] = -1.0;
        g_coinNearX[i] = g_coinNearY[i] = 0.0;
        g_coinNearT[i] = 0;
        g_coinSaid[i] = 0;
        g_coinSeen[i] = 0;
    }
    g_coinProbeMaxX = -1e18;
    for (int b = 0; b < kTouchBits; ++b) g_countSaid[b].store(0);
    for (int i = 0; i < 16; ++i) g_itemSaid[i] = 0;
    // ...and what the cap covered. loadTouchTriggers fills all five, but a
    // level with no triggers file never calls it, so without this the second
    // solve in a process publishes the FIRST level's coverage -- and the
    // UNCOVERED label would name boxes belonging to another level. The
    // declaration says "zero before it runs"; this is what makes that true on
    // the second call (audit AUD-20260920-11).
    g_trigTotal = g_trigRelevantN = g_trigKept = g_trigDroppedRelevant = 0;
    g_trigDroppedBehind = g_trigDroppedAhead = 0;
    g_trigMaxKeptX = 0.0;
    g_trigReported = 0;
    g_bandPath.clear();
    g_bands.clear();
    g_needTrig = 0;
    g_needUnseen = false;
    g_needSkip = 0;
    g_obbAll = true;
    g_touchFromAnchor = false;
    g_trigDump = false;
    g_trigEffect = false;
    g_stopDump = false;
    g_trigWinSel = false;
    g_trigWinNear = -1e18;
    g_spawnRemap = true;
    g_touchRetimeFrom = 0;
    g_flyHazAfterSolid = false;
    g_fgArmLive = false;
    // ...and the fireB tally. The three counters only ever `++` (cli.hpp:2748,
    // 2749, 2760) and nothing zeroes them, so the line printed at cli.hpp:3504
    // is the PROCESS's running total presented as this solve's -- a number that
    // grows every call and is read as if it did not.
    g_fireBCheck = false;
    g_fireBNoTick = 0;
    g_fireBNoBit = 0;
    g_fireBTooEarly = 0;

    // refwatch.hpp -- the reference-trace watch. All per call, and one of them
    // is worse than drift: g_refRows is filled with `g_refRows[t] = r` and
    // never cleared, so a second --refwatch load MERGES two levels' reference
    // rows into one map keyed by tick. The rows that do not collide survive,
    // which is the shape that reads as a plausible reference rather than a
    // corrupt one.
    g_refWatch = false;
    g_refRows.clear();
    g_rjOn = false;
    g_rjRows.clear();
    g_rjAfter = -1;
    for (int k = 0; k < 3; ++k) { g_rjFirst[k] = -1; g_rjMode[k] = -1; g_rjLayersNear[k] = 0; }
    g_rjLayers = 0;
    g_rjOldNode = 0xffffffffu;
    g_rjUse = false;
    g_rjFull = false;
    g_rjJoinT = -1;
    g_refEps = 0.01;
    g_refDriftVy = 0.0;
    g_refDriftY = 0.0;
    g_refParent = -1;
    g_refLostAt = -1;
    for (int i = 0; i < 2; ++i) {
        g_refKidFate[i] = -1;
        g_refKidWhy[i] = "";
        g_refKidKey[i] = 0;
        g_refKidState[i] = State{};
    }
}

}  // namespace dp

