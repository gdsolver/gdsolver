#pragma once
#include "dp/fixup.hpp"
#include "dp/cam_y_obs.hpp"
#include "dp/clearance.hpp"
#include "dp/game_event.hpp"
// ...and the reference watch, whose globals this file clears. It was not in the
// chain: only cli.hpp includes this header, and cli.hpp had already pulled
// refwatch.hpp in, so the omission was invisible until src/mod/dp_bridge.cpp --
// the other caller of resetInvocationState -- failed to compile. refwatch.hpp
// includes frames.hpp and state.hpp and nothing includes reset.hpp except
// cli.hpp, so there is no cycle.
#include "dp/refwatch.hpp"
#include "dp/search_census.hpp"
#include "dp/port_audit.hpp"
#include "dp/cap_audit.hpp"

namespace dp {

// ---- state that must not survive one call into the next -------------------
//
// The solver was written as a process: `leveldp` parses a command line, loads a level, searches,
// prints, exits. Under that assumption a namespace-scope global IS per-solve state, and nothing
// ever had to clear one.
//
// The mod broke that assumption. It calls cliMain in-process (dp/src/dpcore.cpp) dozens of
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
// NOT STATE EITHER: the tables kept across calls on purpose (cli.hpp LevelTextCache,
// GroupLayersCache with its g_groupLayersGen; triggers.hpp TrigRowsCache, GroupMembersCache). Each
// is handed out only when the bytes it was made from are the same bytes, so a hit is what parsing
// them again would give. The ladder's level cache (cli.hpp LadderLevelCache, g_ladder) lives for
// one ladder: cliMain empties it when the ladder ends, and this runs between that ladder's
// attempts, which is exactly what it must survive.
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
    g_bandSrcRows.clear();
    g_bandTrackCam = -1;
    g_bandTrackEnd = -1;
    g_slopeDbg = false;
    g_fxWatchT = -1;
    g_vyWatchT = -1;
    g_vyWriter = 0;
    g_vyWrites = 0;
    g_yWriter = 0;
    g_yWrites = 0;
    g_impulseSite = 0;
    g_impCount = 0;
    g_vyWatchT2 = -1;

    // clearance.hpp -- instrumentation. The tallies are per solve, so a second
    // in-process call must not read the first one's cells.
    g_clearProbe = false;
    g_portAudit = false; g_portAuditStats = PortAuditStats{}; g_portAuditPending.clear();
    g_capAudit = false; g_capAuditEvery = 10; g_capAuditStats = CapAuditStats{};
    g_clear.reset();
    g_goalDiv = GoalDiversity{};

    // constants.hpp -- the measured values, which --flags override per call
    g_hazDbgUid = -1;
    g_camy = false;   // --camy (cam_y_obs.hpp): observation only
    g_camyRest = -1e30; g_camyZoomSeed = -1.0; g_camyOffSeed = 0.0;
    g_camyBand = false;   // --camyband (cam_y_obs.hpp)
    dp::gdcam::g_camNoLive = false;   // --camynolive (cam_model.hpp)
    g_camyObs.reset();
    g_camyLevel.clear(); g_camyEv = CamyPortalEv{}; g_camyStart = -1e30; g_camyStaticSeed.clear();
    g_gravToggle = true;
    g_portalObb = true;
    g_portalObbMode = true;
    g_obbScale = true;
    g_capHoldClass = false;
    g_capHoldPair = false;
    g_shadowSupply = true;
    g_shadowSpiderJump = false;
    g_shadowDual = true;
    g_forceBoxOn = true;
    g_rotTouch = true;
    g_spiderNoExtCol = true;
    g_followTouch = false;
    g_tap2Max = 4;
    g_chanSwitch.clear();
    g_areaAdd = true;
    g_touchOwnOff = true;
    g_tapDisarm = true;
    g_nanSlopeDropped = 0;
    g_areas.clear();
    g_bigSecObjs.clear();
    g_bigSecDyn.clear();
    g_rotBoost = true;
    g_teleportTrig = true;
    g_wideScan = true;
    g_startP2dx = 0.0;
    g_gravTouch = true;
    g_tpTouch = true;
    g_startGmod = std::nan("");
    g_gravAnchorChan = -1;
    g_gravAnchorRev = 0;
    g_gravAnchorX = std::nan("");
    g_tpBack = true;
    g_ctrlOpenT0 = -1;
    g_ctrlOpenX0 = 0.0;
    g_ctrlOpenEnd = std::nan("");
    g_startXhi = std::nan("");
    g_tpPush = true;
    g_tpExitRot = true;
    g_tpExitRots.clear();
    g_tpExitMove = true;
    g_tpExitMoves.clear();
    g_moverExtendVel = false;
    g_shadowRotProbe = false;
    g_canonOrder = 0;
    g_canonShuffle = 0;
    g_worldKey = 0;
    g_bandPair.on = false;
    g_worldTree.on = false;
    g_togTrace = 0;
    dp::gdtrig::g_togTable = dp::gdtrig::TogTable{};
    g_layerDigest2Path.clear(); g_layerBytes2T.clear(); g_ld2LastT = -1; g_ld2Rung = 0;
    if (g_layerDigest2File) { std::fclose(g_layerDigest2File); g_layerDigest2File = nullptr; }
    g_layerDigestPath.clear();
    if (g_layerDigestFile) { std::fclose(g_layerDigestFile); g_layerDigestFile = nullptr; }
    g_ldLastT = -1; g_ldRung = 0;
    g_recAnchorXN = 0;
    g_rotLagN = 0;
    g_rotTailN = 0;
    g_recEndTailN = 0;
    g_recEndTailTried = 0;
    g_recEndTailOnN = 0;
    g_noColOneWay = true;
    g_noColStatic = true;
    g_recTouchGiven = false;
    g_statHoldRecBoxHits = 0;
    g_recTouch.clear();
    g_recTouchBit.fill(-1);
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
    g_resimTrig = TouchMask{};
    g_resimFrame = -1;
    g_groupsEndT = -1;
    g_groupHoldDeath = false;
    kLandTol = 10.0;
    kStickGap = 4.034;
    kRotPerpWin = 150.0;
    kStepDepth = 3.15;
    // The mod calls the solver more than once per process, so a flag left set by
    // the previous level would silently put coin routing on for the next one
    // (the in-process caller's globals are this file's whole subject).
    g_coinRoute = false;
    g_worldGates = false;
    g_coinMaskSeed = 0;
    g_coinSkip = 0;
    g_itemBase.clear();
    g_collect.clear();
    // ...and the two maps the same load fills beside it: loadCollectibles only ever added to the
    // givers, and the coin uids were assigned only under --coins, so in one process each held
    // the previous levels' (and a call without --coins read the last coin call's).
    g_itemGiver.clear();
    g_coinUids.clear();
    kCoinMargin = 0.0;

    // dynamics.hpp
    g_recPhase = TouchMask{};
    g_autoTrig.clear();
    g_autoStop = true;
    g_autoStopBy.clear();
    g_autoDisarm.clear();
    g_dynObbRot = true;
    g_autoTrigFrame = true;
    g_rotated.clear();
    g_rotSpec.clear();
    g_rotTailParts.clear();
    g_trigRaw = false;
    g_xTrack.clear();
    g_xTrackWalkPath.clear();
    g_xTrackW.clear();
    g_xTrackCh.clear();
    g_xTrackChan = false;
    g_xTrackWorld = false;
    g_camRows.clear();
    g_camXtrackRaw.clear();
    g_camFromFile = false;
    g_recLeadsN = 0;
    g_touchRecLeadsN = 0;
    g_lagFitDbg = false;
    g_touchEnteredGiven = false;
    g_touchEntered.clear();
    g_touchEnteredT.clear();
    g_dynDbg = -1;
    if (g_geoDumpF) std::fclose(g_geoDumpF);   // --geodump (dynamics.hpp): print only, per call
    g_geoDumpF = nullptr;
    g_geoT0 = 0;
    g_geoT1 = -1;
    g_geoStride = 1;
    g_formulaDriven = 0;
    g_recProvGiven = false;
    g_recProvRec = g_recProvAnchor = g_recProvReach = -1;
    g_touchRecEntry.fill(-1);
    g_recTouchEnteredGiven = false;
    g_recTouchEnteredT.clear();
    g_recOwnEntry.fill(-1);
    g_formulaRecHit.store(0);
    g_formulaRecMiss.store(0);
    // The rotation router's counts (`rotplace:` prints them). The loader only adds to them, so
    // in-process every call printed the sum over all the calls before it.
    g_rotSeen = 0;
    g_rotRouted = 0;
    // ...and three flags the audit had been carrying as known defects. Two are
    // diagnostics, but g_rotSplit was not (removed: rotation splitting is
    // physics, not instrumentation, and is always on now).
    g_rotCheck = false;
    g_shiftStat = false;
    // ...and the locked box, which the loader finds. Same trap as
    // g_touchMoveTicks: left behind, the next level in a one-session run reads
    // the PREVIOUS level's box index, and a one-session run is exactly the
    // arrangement the serial cold uses.
    g_lockBox = -1;
    g_lockTicks = 0.0;

    // fixup.hpp -- --fixups appends, so without this the file is loaded once per call and the
    // records of every earlier call are still in the list
    g_fixups.clear();
    g_fixupKills.clear();
    g_fixupDeltas.clear();
    g_fixupHits = 0;
    g_fixKillWorld = false;   // constants.hpp
    g_seamKeepOn = false;     // groups.hpp
    g_touchReArm = true;           // constants.hpp
    g_traceHold = false;           // constants.hpp
    g_hsShadow = true;             // constants.hpp
    g_ringSlots = true;            // constants.hpp
    g_ringSlotsKey = true;         // constants.hpp
    g_holdRecBox = true;           // constants.hpp
    g_hsMergeAudit = false;        // constants.hpp
    g_hsMergeDropped = 0; g_hsMergeDroppedDistinct = 0; g_hsMergeReplaced = 0; g_hsMergeReplacedDistinct = 0; g_hsMergeStored = 0;
    g_hsP22 = 1;                   // constants.hpp
    g_traceContact = false;        // constants.hpp
    g_dynKillUids.clear();
    g_fixKillStale = 0;
    for (FixupCensusRow& row : g_fixupCensus) row = FixupCensusRow{};
    g_fixupRotSeen = 0;
    g_fixupCallRotSeen = 0;
    for (FrameRevRow& row : g_frameRevReach) row = FrameRevRow{};

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
    // --coindbg / --supdbg (removed) are set only when passed (cli.hpp), so one call's printout carried into
    // the next in-process call (tools/reset_coverage.py, 2026-10-09)
    g_coinDbgT = -1;
    g_qfoldLo = -1;
    g_qfoldHi = -1;
    g_rotQToggle = false;
    g_rotQOff.clear();
    g_rotQAutoTog.clear();
    g_rotQUidSet.clear();
    g_rotQOn.clear();
    g_rotTrigOff.clear();
    g_rotTrigOn.clear();
    g_touchSeedArg.clear();
    g_touchCensus = false;
    g_touchStops.clear();
    g_anchorRide = true;
    g_gpHandoff.clear();
    g_vetoPhys = false;
    g_resimLadderPartial = false;
    g_resimLadderBound = 1500;   // --rlbound (constants.hpp); 0 is the off arm
    g_resimLadderSpread0 = false;
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
    g_ownsEver = false;
    g_everPayloadOn = false;
    g_everPayloadSet.clear();
    g_everPayloadSet2.clear();
    g_spentRot.clear();
    g_spentPad.clear();
    g_spentPadSeed = true;
    g_ctrlWin.clear();
    g_ctrlWinLag = true;
    g_ctrlWin2.clear();
    g_recInterpSize = true;
    g_recInterpSlow = true;
    g_ballStakeSeed = false;
    g_rotQ64 = true;
    g_partialByX = false;
    g_partialPhys = false;
    g_skyPrune = false;
    g_ladderByX = false;
    g_ladderLevelCache = false;
    g_shadowColDerive = false;
    g_shadowSDiff = false; g_shadowSeedTol = 0.01;
    g_shadowColDumpPath.clear();
    g_shadowQuietDumpPath.clear();
    g_shadowRotBook = false;
    gdcol::g_slActCap = gdcol::kSlAct; gdcol::g_slPruneMargin = gdcol::kSlPruneMargin;
    g_spinSeed = false; g_seedSpinKnown = false; g_seedSpin = 0; g_seedSpinT = -1;
    g_seedHistT0 = -1;
    g_shadowDashAge = false; g_seedDashAgeKnown = false; g_seedDashAge = -1; g_seedDashAgeT = -1; g_seedUnderUid = -1; g_seedUnderT = -1; g_seedUnderUid2 = -1; g_seedUnderT2 = -1;
    g_shGpUid.clear();
    g_startP2Left = 0;
    g_startRHover2 = 0;
    gdcol::g_spiderGrid = true;
    gdcol::g_levelHasEventTrig = true;
    gdev::evChainReset();   // the level's event record (State::evgate's chain) and the events its Event triggers listen for
    gdev::g_supplyEventUids.clear();   // ...and the objects only an Event trigger's chain switches (dynamics.hpp applyTriggersImpl): a one-session run's next level must not meet the last one's uids
    g_sameTick = true;
    g_offAfter = true;
    g_unlinkTrigs.clear();
    g_ctrlTrigs.clear();
    g_pcTrigs.clear();
    g_pcTouchBoxes.clear();
    g_revTrigs.clear();
    g_revTouchBoxes.clear();
    g_ctrlTrigFrom = -1;
    g_holdTapSpans.clear();
    g_startHoldField = -1;
    g_wsMiniR = 2.241; g_wsMiniL = 3.246; g_wsMiniY = 2.1;
    g_startRevOff = g_startRevCarry = std::nan("");
    g_stopJump = true;
    g_reverseTrig = true;

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
    g_forceFields.clear();
    g_forceCircles = 0;
    g_timeWarps.clear();
    g_tpTrigs.clear();
    g_gravTrigs.clear();
    g_gravOff.clear();
    g_gravTrigs2.clear();
    g_seedGmod2Known = false; g_seedGmod2Milli = -1; g_seedGmod2X = 0.0;
    g_seedSlope2 = false;
    g_gravSpawned.clear();
    g_tpSpawned.clear();
    g_gravTouchBoxes.clear();
    g_tpTouchBoxes.clear();
    g_tpBackHi.clear();
    g_tpBackObj.clear();
    g_eventFires.clear();
    g_camFrees.clear();
    g_followYs.clear();
    g_followSecObjs.clear();
    g_zoomTrigs.clear();
    zoomCrossReset(0);
    // bands.hpp: the objrects header's own answer to "does this dump carry the
    // Free Mode / touch / spawn columns". Per level, so it has to be cleared
    // with everything else -- a stale `true` would let the next level in the
    // same process use the band's own height with no gate behind it.
    g_freeModeCol = false;
    g_trigGateCol = false;
    g_staticCamCol = false;
    g_staticCams.clear();
    g_anchorBandB = AnchorBandB{};
    g_flipHeadBoxes.clear();
    g_nojumpBoxes.clear();
    g_dashStopBoxes.clear();
    g_ctrlTouchBoxes.clear();
    g_armBoxes.clear();
    g_slideBoxes.clear();
    g_pressRings.clear();

    // search_key.hpp
    g_topAhead.clear();
    g_topAheadDyn.clear();
    g_shipYq = 0.5;
    g_shipVq = 2.5;
    g_cubeYq = 2.0;
    g_cubeVq = 10.0;
    g_dualFreeQ = 0.125;
    g_keyXq = kDefKeyXq;
    g_keyXqFly = kDefKeyXq;

    // speed.hpp
    kSupportTol = 0.01;
    g_startSpeedMul = 0.0;
    g_dynHazPad = 0.0;
    g_dynSeamGap = 30000;
    g_dynSeamCut = 0;
    g_fixRadiusCollision = 0;
    g_fixGravityBug = 0;
    g_fixNegativeScale = 0;
    g_fixRobotJump = 0;
    g_dynamicLevelHeight = 0;
    g_platformerMode = 0;
    g_maxPlayY = 1e18;
    g_rawValues = kRawValuesDefault;
    g_heldAll = 1;
    g_solidOrdDbgT0 = g_solidOrdDbgT1 = -1;
    g_spentOrb.clear();
    g_spentOrb2.clear();
    g_spentGlobal = true;
    g_spentMap1.clear();
    g_spentMap2.clear();
    g_heldCellCap = 0;
    g_heldCellTwins = 0;
    g_histStatOn = false;
    g_histStatPath.clear();
    g_goalGd = false;
    g_endPortalX = 0.0;
    g_endPortalFlipX = 0;
    g_reverseSync = -1;
    g_enablePlayerSqueeze = 0;
    g_twoPlayer = 0;
    g_portSeenOverflow = 0;
    g_ghostOn = false;
    g_ghostBase = 0;
    g_ghostLvl.clear();
    g_swapBodies = false;
    g_bodyBit = 1;
    g_startAct2 = -1;
    g_twoBodyCall = false;
    g_levelHasDualOn = false;
    g_twoBodyBirthMode = false;
    g_pinRows.clear();
    g_p2Taps = false;
    g_p2TapPress = false;
    g_mustPress.clear();
    g_p2ReplayLvl.clear();
    g_p2ReplayBase = 0;
    g_shiftDbgUid = -1;
    g_shiftDbgDone = false;

    // stairs.hpp -- a stream owned by the call that opened it
    g_snapOut = nullptr;

    // state.hpp
    g_threads = 4;

    // thread_pool.hpp
    g_aliveCap = 16000;
    g_capMap.clear();
    g_memStat = false;
    g_phaseProf = false;
    // progress.hpp's checkpoint channel is NOT reset here: its `enabled`/`cancel` belong to the
    // CALLER -- the mod clears `cancel` before every solve and turns `enabled` on for the session,
    // and a per-invocation reset would silently undo both.
    g_gcNodes = 8000000;
    g_memLimitMiB = 6144;
    g_waveTidy = 0.0;
    g_waveToggle = 0;
    g_startEdge = -1;
    g_glitchDeco = false;
    g_coinWinY = 0.0;
    g_coinWinVy = 0.0;
    g_coinWinLen = 0.0;
    g_ladderOverCapStop = true;
    g_ladderFirstHit = true;
    g_capEnvelope = false;
    g_lossyStat = false;
    g_lossy = LossyStat{};
    g_yBound = 700.0;

    // triggers.hpp
    g_touch.clear();
    g_touchFollowObjs = nullptr;   // pointed into the last call's Level
    // ...and the per-box move length derived from it. Built beside g_touch in
    // cliMain, so it is cleared beside g_touch here: left behind, the next
    // level in a one-session run keys its states against the PREVIOUS level's
    // box durations, and a leak of exactly that shape lived under a green
    // 22/22 for months.
    g_touchMoveTicks.clear();
    g_pressWin.clear();   // same reason: built beside g_touch, keyed by the next level otherwise
    g_actMask = TouchMask{};
    for (auto& f : g_touchFrame) f.clear();
    // kTouchBits, not 32: the array is std::array<int, kTouchBits>, so a cap of
    // 32 left slots 32.. holding the previous level's fire ticks in a
    // one-session run -- the leak this block exists to close.
    for (int b = 0; b < kTouchBits; ++b) { g_touchFireT[b] = -1; g_touchFireEntryOnly[b] = 0; }
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
    // ...and the search census and the twin skip (search_census.hpp). Their tallies live in
    // cliMain; only the switches are global.
    g_searchCensus = 0;
    g_censusLayersOn = false;
    g_censusLayersPath.clear();
    g_autoSpread = false;
    g_autoSpreadOpen.clear();
    g_autoOwnT0 = -1;
    g_autoRun.clear();
    g_twinAudit = false;
    g_minPulse = 0;
    g_wavePulse = 0;
    g_inputGrid = 0;
    g_gridGroundPress = false;
    g_gridGroundPressCube = false;
    g_gridMap.clear();
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
    // the second call.
    g_trigTotal = g_trigRelevantN = g_trigKept = g_trigDroppedRelevant = 0;
    g_trigDroppedBehind = g_trigDroppedAhead = 0;
    g_trigMaxKeptX = 0.0;
    g_trigFirstAheadX = 1e300;
    g_trigReported = TouchMask{};
    g_bandPath.clear();
    g_bands.clear();
    g_needTrig = TouchMask{};
    g_needTrigUids.clear();
    g_needUnseen = false;
    g_airPress = kDefAirPress;
    g_refAdopt = false;
    g_refAdoptSl = false;
    g_refAdoptMem = false;
    g_traceState = false;
    g_refAdoptT = -1;
    g_refAdoptFields.clear();
    g_groupInit.clear();
    g_bootAuto = true;
    g_groupAutoPaths.clear();
    g_groupAutoTL.clear();
    g_needSkip = TouchMask{};
    g_spawnableX.clear();
    g_trigDump = false;
    g_trigEffect = false;
    g_stopDump = false;
    g_trigWinNear = -1e18;
    g_spawnRemap = true;
    g_touchRetimeFrom = 0;
    g_fbForceBox = -1;
    g_fbForceTick = 0;
    g_replayOn = false;
    // ...and the fireB tally. The three counters only ever `++` (cli.hpp:2748,
    // 2749, 2760) and nothing zeroes them, so the line printed at cli.hpp:3504
    // is the PROCESS's running total presented as this solve's -- a number that
    // grows every call and is read as if it did not.
    g_fireBCheck = false;
    g_groupFire = false;
    g_dupBatchCheck = false;
    g_dupBatchLayers = g_dupBatchLeaders = g_dupBatchFollowers = g_dupBatchParents = g_dupBatchChecked = g_dupBatchMismatch = 0;
    for (long long& v : g_dupBatchFireKeyed) v = 0;
    for (long long& v : g_dupBatchRead) v = 0;
    g_supObs = false;
    g_searchMethod = 0;
    g_beamWidth = 2000;
    g_mcRollouts = 300;
    g_mcWindow = 1000;
    g_mcToggles = 30;
    g_mcSteps = 30000000;
    g_mcSafetyS = 600.0;
    g_mcStall = 200000;
    g_mcCall = -1;
    g_searchSeed = 0;
    g_witnessAuto = false;
    g_rolloutFiresAuto = false;
    g_groupXSplitLayers = 0;
    g_groupXSplitMax = 0;
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
    g_rejoinProbe = 5;
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

#include "dp/defaults_profile.gen.hpp"
namespace dp { inline std::string levelCacheProfile() { return defaultsProfile(); } }   // (cli.hpp's --levelcachecheck: defaults_profile.gen.hpp needs the globals reset.hpp is defined after)
