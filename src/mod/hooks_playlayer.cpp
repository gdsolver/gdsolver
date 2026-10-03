// PlayLayer hook: attempt boundaries, checkpoints, death / completion, cleanup.
#include "mod/playlayer_helpers.hpp"

using namespace p1;

class $modify(PlayLayer) {
    // Catch auto-checkpoint creation (cfg `ckpttrace=1`). In 2.208 creation and storage are
    // separate, and storage is delayed/retried until the state is safe, so both are marked
    CheckpointObject* markCheckpoint() {
        auto* ck = PlayLayer::markCheckpoint();
        if (solver::ckpttrace::g_on) noteCkpt("mark", m_player1, (void*)ck, -1);
        return ck;
    }
    CheckpointObject* createCheckpoint() {
        auto* ck = PlayLayer::createCheckpoint();
        if (solver::ckpttrace::g_on) noteCkpt("create", m_player1, (void*)ck, -1);
        return ck;
    }
    // Record how far the clear visual effects got (stopping after a clear is normal, so
    // whether the results screen was reached is the only evidence)
    void showCompleteEffect() {
        writeResult("endscreen: showCompleteEffect");
        PlayLayer::showCompleteEffect();
    }
    void showCompleteText() {
        writeResult("endscreen: showCompleteText");
        PlayLayer::showCompleteText();
    }
    void showEndLayer() {
        writeResult("endscreen: showEndLayer (results screen)");
        PlayLayer::showEndLayer();
    }
    void updateVisibility(float dt) {
        // During fast mode + render skip, the visibility pass is skipped entirely.
        // (1) Performance: its cost is proportional to the total object count
        // (2) Stability: during the fast loop object state changes massively and it trips
        //     an inconsistency and CTDs
        // With render skip the display preparation itself is unnecessary. Physics and
        // collision are independent, via GD's section scheme
        // Note this is NOT renderSuppressed(): F8 (g_renderOff) stops the drawing but must not
        // stop the visibility pass. Skipping it leaves the visibility state stale, and the first
        // visit() after the screen comes back walks over that and crashes -- the fast loop can
        // afford it only because coming back from fast goes through a resetLevel.
        if (g_started && !g_sessionOver && g_cfg.fastdt > 0
            && g_cfg.skipRender && !g_realtimeOverride) return;
        if (g_endzoneBurn) return;   // burning through the end-zone effects: stop visibility too
        // A section solve's search: off, as in every worker's search (secRenderHold).
        if (g_secRenderWas >= 0) return;
        // Old method (visrefresh=1 only): after render skip ends, clobber the visible
        // section bounds to force a recompute. Clobbering makes the routine that adds all
        // sections up to the current position in one frame crash, so it is OFF by default
        // (the current method rebuilds via resetLevel when rendering resumes)
        if (g_visRefresh && g_visRefreshOn) {
            g_visRefresh = false;
            m_leftSectionIndex = m_rightSectionIndex = 0;
            m_bottomSectionIndex = m_topSectionIndex = 0;
        }
        // When visibility does run, SEH guard: if it trips an inconsistency, only that
        // frame's visibility pass is dropped and execution continues (display preparation
        // only; unrelated to physics / determinism)
        this->safeUpdateVisibility(dt);
    }

    void safeUpdateVisibility(float dt) {
#ifdef GEODE_IS_WINDOWS
        __try {
            PlayLayer::updateVisibility(dt);
        } __except (visAvNote(GetExceptionInformation())) {
            logVisibilityCrashSwallowed();
        }
#else
        // No SEH on Android: the pass runs unguarded, and a fault in it ends the game.
        PlayLayer::updateVisibility(dt);
#endif
    }
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        // A new layer can reuse the outgoing layer's address; its object cache cannot.
        grouptrace::reset();
        // A session is configured here for the level the play menu's Start was pressed for, in
        // the mode it chose. Level selection is the game's own UI as is (custom levels included).
        // A session in progress (g_started && !g_sessionOver) (including autorun.cfg
        // launches) is not interfered with. Leftovers of a finished session count as absent
        // (uiConfigureSession resets everything)
        const bool armed = level && g_uiArmedLevel == level;
        if (g_uiArmedLevel && !armed)
            log::info("play menu: armed for another level, entering {} unarmed",
                      level ? level->m_levelID.value() : -1);
        g_uiArmedLevel = nullptr;   // one entry per Start, whatever is decided below
        bool want = (!g_started || g_sessionOver) && g_uiMode > 0 && armed;
        if (want) uiConfigureSession(level->m_levelID.value());
        // Sample the level's own record before it has run a tick, so the session-end line can
        // state whether anything was written into it (see progressDiff). A level a slice swap
        // brings in (level_slice.hpp) is not sampled: the record the line speaks for stays the
        // one of the level the session was started on.
        if (levelslice::g_swapInit) {
            levelslice::g_swapInit = false;
        } else {
            g_progressLevel = level;
            g_progressAtStart = sampleProgress(level);
        }
        // cfg `rngfresh` (see g_rngFresh): the first level keeps the seeds it found and
        // saves them; every later level of the same game starts from those values again.
        if (g_rngFresh) {
            static bool saved = false;
            static long long seeds[3];
            static const uintptr_t kSeedRva[3] = {gdoff::kSeedTriggerRva, gdoff::kSeedVarIndexRva,
                                                  gdoff::kSeedVarTableRva};
            auto* base = reinterpret_cast<unsigned char*>(geode::base::get());
            for (int k = 0; k < 3; ++k) {
                auto* p = reinterpret_cast<long long*>(base + kSeedRva[k]);
                if (!saved) seeds[k] = *p;
                else *p = seeds[k];
            }
            char b[160];
            snprintf(b, sizeof(b), "rngfresh: %s e90=%lld ee0=%lld ef8=%lld",
                     saved ? "restored" : "saved", seeds[0], seeds[1], seeds[2]);
            writeResult(b);
            saved = true;
        }
        // cfg `rngfix` / `rngseed` (see g_rngFix): the seeds the level is built with. resetLevel
        // sets them again before every attempt.
        rngFixApply();
        if (botDriving() && (g_rngFix || g_rngSeedSet)) {
            char b[128];
            snprintf(b, sizeof(b), "rngfix: GD's random seeds fixed on every attempt (ee0=%lld ef8=%lld%s)",
                     g_rngSeedSet ? g_rngSeedEE0 : kRngFixEE0, g_rngSeedSet ? g_rngSeedEF8 : kRngFixEF8,
                     g_rngSeedSet ? ", from cfg rngseed" : "");
            writeResult(b);
        }
        // `swaplevel`: how long the whole swap took, measured from the command to a layer
        // that has its objects. The build of the level string is reported separately, so
        // the two together say where the cost is.
        if (g_swapTiming) {
            const auto t0 = std::chrono::steady_clock::now();
            const bool ok = PlayLayer::init(level, useReplay, dontCreateObjects);
            const auto now = std::chrono::steady_clock::now();
            char b[192];
            snprintf(b, sizeof(b), "swaplevel: layer built in %lld ms, %lld ms since the command "
                     "(objects=%d)",
                     (long long)std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count(),
                     (long long)std::chrono::duration_cast<std::chrono::milliseconds>(now - g_swapT0).count(),
                     level ? (int)level->m_objectCount : -1);
            writeResult(b);
            g_swapTiming = false;
            return ok;
        }
        return PlayLayer::init(level, useReplay, dontCreateObjects);
    }

    // Release session state before either the quit path or a scene exit tears the layer down.
    void cleanupSessionOnExit() {
        // Leaving the layer = session aborted. endSession alone is not enough:
        // if bookkeeping leftovers remain, hooks that do not check !g_sessionOver keep
        // running during subsequent normal play and break behaviour. Full reset on exit
        if (g_started) {
            if (!g_sessionOver) {
                log::info("session ended by level exit");
                endSession("level_exit");
            }
            resetSessionState();
            g_cfg = Config{};
        }
        // Hand the audio engine back untouched: a pitch left over from the spectating notch
        // would follow the player into the menu music
        audio::neutral();
        // ...and take our notification with us. Leaving the level is a scene swap, and one of
        // ours still on screen would follow the player into the menu -- Geode draws them off the
        // director, not the scene -- with nothing left to explain what it refers to.
        notify::clear();
    }

    // Preserve the explicit quit path's cleanup and then let GD leave the level.
    void onQuit() {
        cleanupSessionOnExit();
        PlayLayer::onQuit();
    }

    // Returning to the editor bypasses onQuit; do not keep pointers into the departing scene.
    void onExit() override {
        if (this == PlayLayer::get()) cleanupSessionOnExit();
        if (grouptrace::owns(this)) grouptrace::reset();
        PlayLayer::onExit();
    }

    void resetLevel() {
        // GD's random seeds first, before anything the reset draws from them (see g_rngFix):
        // every attempt and every checkpoint restore of a session starts from the same values.
        rngFixApply();
        // A reset of an old layer (embers awaiting the scene swap) must not touch the books
        if (this != PlayLayer::get()) { PlayLayer::resetLevel(); rngFixAfterReset(this); restoreProgress(); return; }
        // A section-solver restore is "inside the search", so it must not touch the mod's
        // bookkeeping at all. Letting it through runs a grouptrace rebuild, POI rebuild and
        // retry logging on every restore, turning a restore that should take 2ms into tens
        // of ms and mixing up the recordings too.
        if (secsolve::g_active) { PlayLayer::resetLevel(); rngFixAfterReset(this); restoreProgress(); return; }
        // GD can restart after p2 dies while its dual flag is already off (t=7479 on a custom
        // level). Confirm the failed flight here, BEFORE reset erases its anchors and geometry.
        const auto missed = solver::g_unbookedP2Death.takeForReset(
            g_started && !g_sessionOver && g_cfg.dpSolve && !g_dpShowSolution
                && !g_cfg.noDeath && !secsolve::g_on && !dpsolve::g_stop
                && !dpsolve::g_running.load() && !dpsolve::g_deepActive,
            solver::g_deathBooked);
        if (missed.tick >= 0) {
            writeResult("attempt_end: GD restarted after an unbooked non-dual p2 death at t="
                        + std::to_string(missed.tick) + " - handing the failed flight to repairs");
            // The reset proves failure, not that p1 collided: do not learn a phantom p1 kill.
            bookAttemptEnd({missed.x, missed.y}, solver::AttemptEndKind::ConfirmedP2Reset,
                           "unbooked_p2_reset", missed.tick);
        }
        // A newly installed plan also fulfils a stall's deferred reset request.
        g_stallResetPending = false;
        hookdepth::Guard hg(hookdepth::RESET);
        stallwatch::Mark sm(stallwatch::RESET);
        // Early heap-corruption check (cfg `heapcheck=N`). Fired at attempt boundaries so
        // the corrupted interval is bracketed by attempt numbers
        if (g_heapCheckEvery > 0 && g_started && !g_sessionOver) {
            static long long s_lastCheck = 0;
            if (solver::g_totalAttempts - s_lastCheck >= g_heapCheckEvery) {
                s_lastCheck = solver::g_totalAttempts;
                auto t0 = std::chrono::steady_clock::now();
                int heaps = 0; long long blocks = 0;
                bool ok = postmortem::heapOk(&heaps, &blocks);
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                if (!ok) {
                    // THIS IS THE POINT: before crashing, leave an explicit note that it is
                    // corrupted
                    postmortem::writeRaw("HEAP CORRUPTED (detected by heapcheck)");
                    writeResult("heapcheck: CORRUPT at attempt "
                        + std::to_string(solver::g_totalAttempts)
                        + " tick=" + std::to_string((long long)g_tick));
                    g_heapCheckEvery = 0;   // silent from now on (only the first hit matters)
                } else {
                    static int s_logs = 0;
                    if (++s_logs <= 3 || (solver::g_totalAttempts % (g_heapCheckEvery * 20)) == 0)
                        writeResult("heapcheck: ok at attempt "
                            + std::to_string(solver::g_totalAttempts)
                            + " (" + std::to_string(ms) + "ms, heaps=" + std::to_string(heaps)
                            + " blocks=" + std::to_string(blocks) + ")");
                }
            }
        }
        auto resetT0 = std::chrono::steady_clock::now();
        struct ResetTimer {
            std::chrono::steady_clock::time_point t0;
            ~ResetTimer() {
                solver::g_resetNanos += std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                ++solver::g_resetCalls;
            }
        } resetTimer{resetT0};
        solver::g_deathBooked = false;  // the next run's death is booked afresh
        speedgate::newAttempt();   // portal detection restarts every attempt
        // Clean start on the session's first run: purge practice mode / checkpoint
        // leftovers from the previous session
        if (g_forceCleanStart && g_started && !g_sessionOver) {
            g_forceCleanStart = false;
            if (m_checkpointArray) {
                while (m_checkpointArray->count() > 0) this->removeCheckpoint(false);
            }
            if (m_isPracticeMode) this->togglePracticeMode(false);
        }
        // Detect the practice-mode checkpoint restore path (for the method-B verification
        // harness)
        bool ckptRestore = m_isPracticeMode && m_checkpointArray
            && m_checkpointArray->count() > 0 && g_ckpt;
        solver::g_injThisAttempt = 0;
        solver::g_restoreTickDbg = 0;
        solver::g_prevTickX = -1e9f;
        orbtrace::reset(); // do not carry the previous attempt's contact tick over
                           // (orb pointers are reused)
        if (!ckptRestore) {
            ++g_attempt;
            solver::g_cpInjected = false;   // the state-injection probe runs once per attempt
            g_injectNext = 0;               // analysis injections are also re-read from the
                                            // start of the attempt
            g_stopFired = false;
            g_nextInput = 0;
            g_attemptFed = false;
            g_nextToggle = 0;
            g_tick = 0;
            g_gameFrame = 0;   // rotation does not carry across attempts
            anchors::onAttemptStart();   // the re-anchor record is per attempt
            padseed::reset();            // ...and so are the pads it seeds (cfg dpspentpad)
            ringseed::reset();           // ...and the rings (dp --spentorb)
            // ...and the gravity portals and touch triggers (cfg portalpayload / touchpayload).
            // Kept per level, their first-wins ticks came from whichever attempt got there
            // first: lv20's bootstrap record runs the level to the end on its own route and
            // reached x=22,747 at t~14,446, so an anchor at t=15,433 (x=22,332) carried that
            // portal as spent, the model did not fire it, and GD did.
            portalseed::reset();
            touchseed::reset();
            itermap::onAttemptStart();   // ...and so is the seek bar's tick -> x record
            // The HUD's best coin count, before the attempt that got them is cleared.
            solver::g_coinBest = std::max(solver::g_coinBest, solver::coinsThisAttempt());
            solver::g_coinPickupTick.assign(solver::g_coins.size(), -1);
            // GD's verdict is per attempt for the same reason ours is. GD's own
            // "already collected" dictionary is NOT cleared here -- that is its
            // business, and clearing it would be changing the game -- so on a
            // second attempt hasUniqueCoin may suppress the call; that shows up
            // as gd=-1 against ours>=0 and is a real fact about the instrument,
            // not something to paper over.
            solver::g_coinGdTick.assign(solver::g_coins.size(), -1);
            solver::g_coinGdUnmatched = 0;
            solver::g_coinMissFired = false;   // the miss request is one per attempt
            solver::g_coinMissIdx = -1;
            solver::g_endTriggerFiredTick = -1;
            for (auto& lg : solver::g_coinPosLog) lg.clear();
            route::onAttemptStart();   // cfg routeprereq: the switches are per attempt too
            solver::g_itemCounts.clear();      // ...and so are GD's item counters
            solver::g_coinLiveSaid.assign(solver::g_coins.size(), 0);
            g_attemptStart = std::chrono::steady_clock::now();
            // The moving-geometry recording is rewritten every attempt (leaving the previous
            // attempt's rows would put two rects on the same tick)
            if (grouptrace::g_on) {
                if (grouptrace::owns(this)) grouptrace::restart();
                else grouptrace::build(this);
            }
            // Latch what KIND of attempt this is while the answer is still true. The recorder can
            // retire mid-attempt (its tick budget), and levelComplete asks afterwards -- see the
            // note on g_recordAttempt.
            dpsolve::g_recordAttempt = dpsolve::g_deepActive;
            // cfg cpflight: this attempt's checkpoint set. Only the loop's own flights take them --
            // not a recording pass, a check flight, a section search's replay, a slice's flight on
            // the level itself, the probe or its control, or the showing.
            cpflight::g_forceHead = false;
            cpflight::onAttemptStart(g_attempt,
                g_started && g_cfg.dpSolve && !g_dpShowSolution && !secsolve::g_on
                && !dpsolve::g_deepActive && !dpsolve::g_ckFlying
                && levelslice::g_phase != levelslice::Verifying
                && levelslice::g_phase != levelslice::RefFlight
                && !(cpflight::g_starting && cpflight::g_starting->probe)
                && !cpflight::g_controlStarting);
            if (cpflight::g_controlStarting) cpflight::startControl();
            // Required for coinMode / clearance: a plain replay never runs buildPois, and
            // with g_coins / g_solids empty neither pickup detection nor sample collection
            // ever runs
            if ((g_cfg.coinMode || clearance::g_on || g_cfg.dpSelfTest || g_cfg.dpSolve)
                && !solver::g_poisBuilt) {
                solver::buildPois(this);
                solver::g_poisBuilt = true;
                // cfg routeprereq: what each coin needs entered first (solver/route.hpp), read off
                // the same objects before the first solve can ask.
                if (g_cfg.routePrereq && g_cfg.coinRoute)
                    route::build(this, solver::g_coinObjs, [](const route::Prereq& p) {
                        char b[224];
                        snprintf(b, sizeof(b), "route: coin %d needs uid %d (id %d) entered at "
                                 "(%.0f,%.0f) %.0fx%.0f -- it switches on %s uid %d", p.coin,
                                 p.boxUid, p.boxId, (double)p.bx, (double)p.by,
                                 (double)(2 * p.bhw), (double)(2 * p.bhh),
                                 p.gate ? "the gate object" : "the coin itself", p.watchUid);
                        writeResult(b);
                    });
                if (g_cfg.dpSelfTest) dpSelfTest(this);
                // Stage B: solve this level in-process, then replay what comes back
                if (g_cfg.dpSolve) dpsolve::start(this);
                writeResult("pois: " + std::to_string(solver::g_pois.size()) + " orbs, "
                    + std::to_string(solver::g_coins.size()) + " coins");
            }
        }
        if (g_cfg.cbs >= 0) m_clickBetweenSteps = (g_cfg.cbs == 1);
        if (g_cfg.cos >= 0) m_clickOnSteps = (g_cfg.cos == 1);
        ev("resetLevel", m_clickBetweenSteps, ckptRestore);
        // Leftover completion flags on retry are invisible from outside, so record before/after
        if (g_sessionOver || g_retryDone) {
            char rb[192];
            snprintf(rb, sizeof(rb),
                "retry-reset: before completed=%d locked=%d dead=%d tick=%lld",
                m_hasCompletedLevel ? 1 : 0,
                m_player1 ? (m_player1->m_isLocked ? 1 : 0) : -1,
                m_player1 ? (m_player1->m_isDead ? 1 : 0) : -1, (long long)g_tick);
            writeResult(rb);
        }
        PlayLayer::resetLevel();
        rngFixAfterReset(this);   // the trigger seed GD just drew from the clock (see g_rngFix)
        // cfg secshadersig (print only): the shader layer's hierarchy after a real reset -- the
        // first of every layer, and the first after a section search (secsolve::shaderSig).
        if (secsolve::g_shaderSig) {
            static const void* s_sigLayer = nullptr;
            if (s_sigLayer != this) {
                s_sigLayer = this;
                writeResult("shadersig: first reset of level " + std::to_string(g_cfg.levelId)
                            + " " + secsolve::shaderSig(this));
            } else if (secsolve::g_shaderSigPending) {
                writeResult("shadersig: first reset after the search "
                            + secsolve::shaderSig(this));
            }
            secsolve::g_shaderSigPending = false;
        }
        // cfg cpflight: a flight from a checkpoint takes the loop's books up to it from the
        // attempt that took it -- its recording included, `init,` lines and all.
        const bool cpStart = !ckptRestore && cpflight::g_starting;
        if (cpStart) cpflight::splice(*cpflight::g_starting);
        else if (!ckptRestore) cpflight::afterHeadReset(this);   // the carry's node prediction, checked
        // The objects' on/off as the reset left them, before the first update (see
        // grouptrace::snapshotInit for why this phase and not the recording's first row).
        if (!ckptRestore && !cpStart && grouptrace::g_on && grouptrace::owns(this))
            grouptrace::snapshotInit();
        // resetLevel bumps the LEVEL's attempt counter inline (no call to hook), so put the
        // record back here -- see restoreProgress.
        restoreProgress();
        hitbox::onLevelReset();   // the nodes it hid are gone; re-apply against the new scene
        // Clear the clear-effects flag: if m_levelEndAnimationStarted stays set, GD does not run
        // processCommands, and after the reset only rendering runs and it hangs. The level has
        // just been rebuilt, so the effects belong to the run that ended — the flag is stale
        // whoever asked for the reset.
        //
        // [2026-08-23] This used to be inside the `session over / retry` test, so any OTHER path
        // that restarts a level after a clear inherited the hang: F7 after the replay finished,
        // and the solve loop's showing of its solution, which restarts the level from underneath
        // the results screen by design. The condition below still decides what gets LOGGED; the
        // clearing itself is unconditional.
        const bool wasEndAnim = m_levelEndAnimationStarted;
        if (wasEndAnim) m_levelEndAnimationStarted = false;
        if (g_sessionOver || g_retryDone || wasEndAnim) {
            char rb[224];
            snprintf(rb, sizeof(rb),
                "retry-reset: after  completed=%d locked=%d dead=%d x=%.1f "
                "endAnimWas=%d (cleared)",
                m_hasCompletedLevel ? 1 : 0,
                m_player1 ? (m_player1->m_isLocked ? 1 : 0) : -1,
                m_player1 ? (m_player1->m_isDead ? 1 : 0) : -1,
                m_player1 ? m_player1->getPositionX() : -1.f,
                wasEndAnim ? 1 : 0);
            writeResult(rb);
        }
        // (Not for a flight from a checkpoint: the layer puts its hold back as the checkpoint had it.)
        if (!ckptRestore && !cpStart && g_started && g_cfg.blockInput) {
            // Guard against a leftover hold: if the previous attempt ends while pressed, the
            // button carries over into tick 0 of the next attempt and it jumps continuously
            // from the very start (breaking reproduction of the same plan)
            g_injecting = true;
            this->handleButton(false, 1, true);
            g_injecting = false;
        }
        if (g_started && g_finishedAttempts >= g_cfg.maxAttempts) {
            endSession("max_attempts");
        }
    }

    // Ignore the automatic pause on focus loss during an automated session.
    // pauseGame(unfocused=true) is called when the window loses focus, and every background
    // worker gets stopped as collateral (while paused no update arrives, and from outside it
    // is indistinguishable from a hang).
    // A manual pause (Escape, unfocused=false) is let through
    void pauseGame(bool unfocused) {
        // GD adds the pause menu to this layer's parent, and a layer without one is a crash
        // (read at pauseGame+0x2aa). That only happens to a layer that has lost its scene --
        // which the slice swap once caused (level_slice.hpp, sceneBusy) -- and the game must not
        // go down for it.
        if (!this->getParent()) {
            writeResult("pause: refused - the layer has no scene to put the pause menu in");
            log::warn("pause: refused - the layer has no scene to put the pause menu in");
            return;
        }
        if (unfocused && g_started && !g_sessionOver) {
            ++g_unfocusPauseBlocked;
            if (g_unfocusPauseBlocked <= 3)
                writeResult("pause: ignored an unfocus-pause (automated session, n="
                    + std::to_string(g_unfocusPauseBlocked) + ")");
            return;
        }
        PlayLayer::pauseGame(unfocused);
    }

    void storeCheckpoint(CheckpointObject* checkpoint) {
        PlayLayer::storeCheckpoint(checkpoint);
        // Observation of auto-checkpoint placement only (cfg `ckpttrace=1`)
        if (solver::ckpttrace::g_on)
            noteCkpt("store", m_player1, (void*)checkpoint, -1);
    }

    // Finalise the moving-geometry recording and tell result.txt which attempt it belongs
    // to. MUST BE CALLED BEFORE the line announcing the run's outcome (`death:` /
    // `complete:`): the reader learns the end of the run from that line and reads
    // grouptrace_last.txt right after. Leaving the finalisation to GD's timing (our own
    // resetLevel, which runs ~1 s after death) makes it depend on polling timing whether the
    // reader grabs the current attempt or the previous one. rows=0 means "_last was not
    // updated"
    void rollGroupTrace() {
        ::rollGroupTrace();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        // A layer on its way out still dies, and records like any other (see NoRecordGuard).
        // Measured on a slice swap: the level's own layer, left running for the frames the
        // copy's scene takes to come up, died over and over, and one death reached GD's record
        // block -- four writes into the level's record, each stopped only by the hooks.
        if (this != PlayLayer::get()) {
            NoRecordGuard nr(this);
            PlayLayer::destroyPlayer(player, object);
            return;
        }
        // cfg killersite=1: built before the original call, written after it (see below).
        char siteBuf[256];
        bool siteArmed = false;
        // A death during the section solver is just "a branch died". Running the mod's death
        // bookkeeping (closing the attempt, effects, retry) breaks the search, so only the
        // plain death is let through.
        // A death during the section solver is just "a branch died". Running the mod's death
        // bookkeeping (closing the attempt, effects, retry) breaks the search.
        // Moreover, in the search body itself (`g_noKill`) NOT EVEN the plain death is let
        // through: actually dying puts the level into the attempt-over state and physics
        // stops, and reviving it needs a checkpoint restore. Just raising a flag and passing
        // through means only that one branch is discarded.
        // Let GD itself name who killed the player (cfg `hitboxtrace=1`, `killer:` lines).
        // Inferring the culprit from nearby rects cannot distinguish objects that were moved,
        // objects misfiled by type, or deaths that are not collisions at all (self-kill,
        // push-out). GD holds the killer in an argument, so asking settles it in one shot.
        // ...also during dpsolve: one line per death, and the repair loop's
        // whole diagnosis problem is "which object did GD hit that the model
        // does not know about". (hitboxtrace's per-tick hbox/dmg flood stays
        // opt-in; this is not that.)
        //
        // EXCEPT GD's OWN ANTI-CHEAT SPIKE. GD calls destroyPlayer with
        // m_anticheatSpike to check that the call still kills, and repeats it
        // every tick while that check has not passed -- which is every tick of a
        // run with nodeath on, because the return below never lets the call reach
        // GD. The bootstrap record of a custom level wrote 30,065 of these
        // lines (lv22's cold, 12,520), each one an open/append/close of
        // result.txt: 11% of the record's main thread, sampled (2026-09-25).
        // The spike names no culprit, so it is counted (g_anticheatCalls, on the
        // session-end `anticheat:` line) and not written. Nothing else changes:
        // the call still goes wherever it went before.
        // Native 2.2081 compares object UIDs, not pointer identity (RVA 0x3b39d0).
        const bool anticheat = object && m_anticheatSpike
            && object->m_uniqueID == m_anticheatSpike->m_uniqueID;
        if (anticheat) ++solver::g_anticheatCalls;
        if ((g_cfg.hitboxTrace || g_cfg.dpSolve) && g_started && !g_sessionOver
            && player && !anticheat) {
            char kb[320];
            snprintf(kb, sizeof(kb),
                "killer: t=%lld who=%s py=%.3f pvy=%.3f px=%.3f obj=%s uid=%d "
                "id=%d type=%d ox=%.3f oy=%.3f stateTick=%lld phase=%s",
                (long long)g_tick,
                player == m_player1 ? "p1" : player == m_player2 ? "p2" : "?",
                player->getPositionY(), (double)player->m_yVelocity,
                player->getPositionX(), object ? "yes" : "NULL",
                object ? (int)object->m_uniqueID : -1,
                object ? (int)object->m_objectID : -1,
                object ? (int)object->m_objectType : -1,
                object ? object->getPositionX() : 0.f,
                object ? object->getPositionY() : 0.f,
                solver::attemptStateTick(g_tick),
                solver::g_commandStateTick >= 0 ? "command" : "physical");
            writeResult(kb);
            // cfg killersite=1: WHERE IN GD THE CALL CAME FROM. `killer:` above
            // names the object, and GD passes null for every rule that is not a
            // collision -- so an object-free death is reported as "something
            // killed you" and nothing more. The call site separates them.
            //
            // The stack is walked rather than read from _ReturnAddress(): this
            // body is reached through Geode's detour, so the immediate return
            // address is inside the generated handler and not in GD at all.
            // Frames outside the game module are dropped, which leaves the
            // chain of GD callers in order.
            //
            // ONE LINE PER DEATH, NOT PER CALL. The first version wrote here, for
            // every call, and choked a repair loop on its first minutes: 22,151
            // lines, five a tick, and not one replay finished. Those calls come
            // from runs that cannot die (the recorder's no-death pass, the section
            // solver's no-kill search) sitting in a hazard, and from calls on a
            // body that is already dead. So the line is only BUILT for a call that
            // could kill, and only WRITTEN after the original call, when the body
            // went from alive to dead -- the same transition the gatetrace
            // `killer:` line below waits for.
            if (g_cfg.killerSite && !player->m_isDead && !secsolve::g_noKill
                && !secsolve::g_active && !g_cfg.noDeath) {
#ifdef GEODE_IS_WINDOWS
                static uintptr_t base = 0, size = 0;
                if (!base) {
                    base = (uintptr_t)GetModuleHandleW(nullptr);
                    auto* dos = (IMAGE_DOS_HEADER*)base;
                    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
                    size = nt->OptionalHeader.SizeOfImage;
                }
                char* sb = siteBuf;
                constexpr int kSb = (int)sizeof siteBuf;
                int o = snprintf(sb, kSb, "killsite: t=%lld obj=%s rva=",
                                 (long long)g_tick, object ? "yes" : "NULL");
                int kept = 0;
                // CaptureStackBackTrace returns only this frame here: Geode's
                // trampoline has no unwind info, so the walk stops at it --
                // pfgstk prints the same two `?` addresses for the same reason.
                // Scan the stack for values lying inside the game module
                // instead, the way postmortem.hpp does after a stack overflow.
                // These are CANDIDATES in stack order, not a call chain: a slot
                // can still hold a return address from an earlier, deeper call.
                ULONG_PTR sp = (ULONG_PTR)&kept;
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery((LPCVOID)sp, &mbi, sizeof mbi)) {
                    ULONG_PTR end = (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
                    if (end > sp + 0x4000) end = sp + 0x4000;
                    for (ULONG_PTR p = sp;
                         p + 8 <= end && kept < 6 && o < kSb - 20;
                         p += 8) {
                        const ULONG_PTR v = *(ULONG_PTR*)p;
                        if (v < base || v - base >= size) continue;
                        o += snprintf(sb + o, kSb - o, "%s%llx",
                                      kept++ ? "," : "",
                                      (unsigned long long)(v - base));
                    }
                }
                if (!kept) snprintf(sb + o, kSb - o, "(none in module)");
#else
                // The stack scan reads the PE image and Windows' memory map; not on Android.
                snprintf(siteBuf, sizeof siteBuf, "killsite: t=%lld obj=%s rva=(unavailable)",
                         (long long)g_tick, object ? "yes" : "NULL");
#endif
                siteArmed = true;
            }
            // The same verdict, latched for the iteration map (itermap.hpp). GD holds the killer
            // in an argument at this instant and nowhere afterwards, and the recorder's last row
            // is a tick short of the death -- so this is the only place the mark's y and the
            // object that put it there can both be had.
            if (player == m_player1)
                itermap::latchKiller(solver::attemptStateTick(g_tick), player->getPositionY(),
                                     object ? (int)object->m_objectID : -1,
                                     object ? (int)object->m_uniqueID : -1);
        }
        if (secsolve::g_active && secsolve::g_killLog && player == m_player1) {
            char kb[220];
            snprintf(kb, sizeof(kb),
                     "seckill: d=%d py=%.3f pvy=%.3f px=%.1f obj=%s uid=%d "
                     "id=%d ox=%.1f oy=%.1f",
                     secsolve::g_depth, player->getPositionY(),
                     (double)player->m_yVelocity, player->getPositionX(),
                     object ? "yes" : "NULL", object ? object->m_uniqueID : -1,
                     object ? object->m_objectID : -1,
                     object ? object->getPositionX() : 0.f,
                     object ? object->getPositionY() : 0.f);
            writeResult(kb);
        }
        // The two returns below (the section search's no-kill, cfg nodeath) keep GD's own
        // destroyPlayer from running, and with it GD's anti-cheat check. For its spike GD returns
        // early if player 1 is locked or the layer's player has died, and otherwise sets
        // m_damageVerified and returns without killing (PlayLayer::destroyPlayer, 0x3b39d0).
        // Swallowed, the check never passes: GD puts the spike back on player 1 every substep, and
        // every substep it is hit checkCollisions returns before postCollision. Measured with lv16's
        // solution, nodeath=1 against nodeath=0: 18,867 spike calls against 13, and the trajectory
        // apart from t=3,078. So those paths do for the spike what GD does, and nothing more
        // (cfg anticheatpass, on). Not a death either way.
        if (anticheat && g_cfg.antiCheatPass && (secsolve::g_noKill || g_cfg.noDeath)) {
            NoRecordGuard nr(this);
            PlayLayer::destroyPlayer(player, object);
            return;
        }
        if (secsolve::g_noKill) {
            if (g_cfg.secDriftWhere && !secsolve::g_died) {
                secsolve::g_diagDeathCaller = player == m_player1 ? 1 : player == m_player2 ? 2 : 0;
                secsolve::g_diagKiller = object ? (int)object->m_uniqueID : -1;
                secsolve::g_diagAnticheat = anticheat;
            }
            if (player == m_player1 || player == m_player2) secsolve::g_died = true;
            return;
        }
        if (secsolve::g_active) {
            NoRecordGuard nr(this);
            PlayLayer::destroyPlayer(player, object);
            return;
        }
        hookdepth::Guard hg(hookdepth::DESTROY);
        stallwatch::Mark sm(stallwatch::DESTROY);
        // Observation-only no-death (cfg `nodeath=1`). Hitboxes stay active, so portals and
        // triggers fire normally and the recording reaches the end in a single run
        if (g_cfg.noDeath) return;
        // Count which player a death in a dual section came from (diagnostic).
        // COUNTED AFTER THE ORIGINAL CALL, for the same reason the killer: line
        // below is written there: `!wasDead` alone counts every destroyPlayer GD
        // makes, and GD makes anti-cheat calls that kill nobody. Measured on
        // calib_slopespike_wave, which completes at 100% with no `death:` line at
        // all and still reported `deaths: p1=6`. So this counter was over the
        // population "destroyPlayer was called", not "the player died" -- the
        // defect the killer: line had until 3576515, left behind in the counter.
        bool wasDead = player->m_isDead;
        const int caller = player == m_player1 ? 1 : player == m_player2 ? 2 : 0;
        const solver::RunDeathState before{
            m_player1 && m_player1->m_isDead, m_player2 && m_player2->m_isDead,
            m_gameState.m_isDualMode};
        const auto flightPos = m_player1 ? m_player1->getPosition() : player->getPosition();
        // Direct evidence of what killed the player (only while measuring gatetrace).
        // The line is BUILT here, before the original call, because px/py move inside
        // it -- and WRITTEN after it, only for a call that actually put the player in
        // the dead state. GD reaches destroyPlayer with anti-cheat pseudo-calls that
        // kill nobody (the transition test below is the same one), and a line emitted
        // for those reports "destroyPlayer was called", not "the player died": one
        // replay opened with thirteen of them and carried the name of an object at
        // x=0 to a death 15,000px later. Both bodies are named as well -- a dual
        // section's p2 death ends the run just as p1's does, and restricting the line
        // to p1 discarded the only record of who GD blamed there.
        char kb[280];
        const bool gateLine = !wasDead && solver::g_gateTrace;
        if (gateLine) {
            const char* who = player == m_player1 ? "p1"
                            : player == m_player2 ? "p2" : "?";
            if (object) {
                // Emit BOTH the position and the hitbox. For an object moved by a group,
                // getPosition may stay at its entry value while only the rect moves (the
                // spike uid 13495 in lv20 has position 24,401 while its rect is at 24,842),
                // and looking at the position alone gives the unreadable story "killed by an
                // object 441px away"
                auto orr = object->getObjectRect();
                snprintf(kb, sizeof(kb),
                    "killer: tick=%lld who=%s uid=%d id=%d type=%d ox=%.1f oy=%.1f "
                    "rect=%.2f,%.2f,%.2f,%.2f on=%d px=%.1f py=%.1f",
                    (long long)g_tick, who, object->m_uniqueID, object->m_objectID,
                    (int)object->m_objectType,
                    object->getPositionX(), object->getPositionY(),
                    orr.origin.x, orr.origin.y, orr.size.width, orr.size.height,
                    (object->m_isGroupDisabled || object->m_isGroupDisabledTemp)
                        ? 0 : 1,
                    player->getPositionX(), player->getPositionY());
            }
            else
                snprintf(kb, sizeof(kb),
                    "killer: tick=%lld who=%s (no object) px=%.1f py=%.1f",
                    (long long)g_tick, who,
                    player->getPositionX(), player->getPositionY());
        }
        {
            // GD records the level's percentage from inside destroyPlayer; the guard makes it
            // take its own no-record path (see NoRecordGuard).
            NoRecordGuard nr(this);
            PlayLayer::destroyPlayer(player, object);
        }
        const solver::RunDeathState after{
            m_player1 && m_player1->m_isDead, m_player2 && m_player2->m_isDead,
            m_gameState.m_isDualMode};
        if (g_started && !g_sessionOver && g_cfg.dpSolve && !g_dpShowSolution
            && !anticheat && !solver::g_deathBooked)
            solver::g_unbookedP2Death.observe(before, after, caller, solver::attemptStateTick(g_tick),
                                             flightPos.x, flightPos.y);
        // ...and now the call has said whether it killed anybody.
        if (gateLine && player->m_isDead) writeResult(kb);
        if (siteArmed && !wasDead && player->m_isDead) writeResult(siteBuf);
        if (!wasDead && player->m_isDead) {
            if (player == m_player1) ++solver::g_deathsP1;
            else if (player == m_player2) ++solver::g_deathsP2;
            else ++solver::g_deathsOther;
        }
        restoreProgress();
        // In dual mode a p2 death also ends the run (restricting to p1, sections where only
        // p2 dies never get booked and the run spins idle). The position used is p1's: all
        // recordings are p1-based, and the two dual bodies share the same x (y is mirrored)
        const bool isP1 = caller == 1;
        const bool isP2Dual = caller == 2 && before.dual;
        // Exclude anti-cheat pseudo-calls: record only on an actual transition into the
        // dead state
        if (solver::deathEndsAttempt(before, after, caller) && !solver::g_deathBooked) {
            auto pos = (isP1 || !m_player1) ? player->getPosition()
                                            : m_player1->getPosition();
            if (isP2Dual) {
                static int s_p2logs = 0;
                if (++s_p2logs <= 5)
                    writeResult("dual: booked a p2 death as the run end (tick="
                        + std::to_string((long long)g_tick)
                        + " x=" + std::to_string((int)pos.x) + ")");
            }
            bookAttemptEnd(pos, g_stallResetPending ? solver::AttemptEndKind::Forced
                                                   : solver::AttemptEndKind::Collision, "stall");
        } else if (g_started && !g_sessionOver && g_cfg.dpSolve && !anticheat
                   && (caller == 1 || caller == 2) && !solver::g_deathBooked
                   && solver::g_deathMissLogs < 8) {
            ++solver::g_deathMissLogs;
            char db[224];
            snprintf(db, sizeof(db), "killcheck: t=%lld caller=p%d no alive-to-dead transition "
                     "p1=%d->%d p2=%d->%d dual=%d->%d; not booked",
                     (long long)g_tick, caller, before.p1, after.p1,
                     before.p2, after.p2, before.dual, after.dual);
            writeResult(db);
        }
    }

    void levelComplete() {
        if (this != PlayLayer::get()) {
            NoRecordGuard nr(this);
            PlayLayer::levelComplete();
            return;
        }
        // A completion the attempt did not earn. Every classic completion is the last action of
        // the end animation, which checkForEnd starts after raising m_levelEndAnimationStarted,
        // and only a reset lowers that byte -- so one arriving with the byte down was started for
        // a world that has since been reset. And during a section search the world is the
        // search's: the frame between two slices asks checkForEnd on whatever branch was left in
        // the player (secStepBegin, hooks_gamelayer.cpp). Measured on a custom level: taken for
        // the session's clear, it filed the plan the search had started from -- which died short
        // of the end -- skipped the slice's flight on the level itself, and a harness counted it
        // as cleared.
        if (g_started && !g_sessionOver && !m_isPlatformer
            && (secsolve::inFlight() || secsolve::g_active || !m_levelEndAnimationStarted)) {
            const bool during = secsolve::inFlight() || secsolve::g_active;
            writeResult(std::string("complete: refused - ")
                        + (during ? "raised during a section search"
                                  : "the attempt never started the end animation")
                        + " (x=" + std::to_string((int)(m_player1 ? m_player1->getPositionX() : -1.f))
                        + " tick=" + std::to_string(g_tick) + ")");
            return;
        }
        hookdepth::Guard hg(hookdepth::COMPLETE);
        stallwatch::Mark sm(stallwatch::COMPLETE);
        ev("levelComplete");
        // A completion raised before the attempt has run a single tick belongs to the attempt
        // before it. Measured on lv2 with checkpoint flights on (cfg `dpcheck`, 2026-09-19):
        // `complete: attempt=14 step=0 tick=0 x=347` as the loop started its first plan, refused
        // as a false clear and filed as a death at t=0, which left the ladder nothing to stand
        // on. The likely source (inferred from the checkpoint layers, not traced): the job's
        // last flight was held at t=19,200, inside the end portal's run-in (the level clears at
        // 19,272), and its completion arrived after the restart. No tick has been flown, so
        // there is nothing to judge.
        if (g_cfg.dpSolve && g_cfg.dpCheck && g_started && g_tick == 0) {
            writeResult("dpsolve:   [check] ignoring a completion raised before the attempt's "
                        "first tick - it belongs to the attempt before");
            return;
        }
        // False-clear detection: depending on session state GD's levelComplete can be called
        // mid-level. So that "a clear while not near the end" can be judged mechanically,
        // always record the x at the moment the guard looks (a different moment from the x
        // in the `complete:` line)
        if (g_started && m_player1) {
            float lm = solver::g_levelMaxX;
            if (lm <= 0 && m_objects) {
                for (auto* obj : CCArrayExt<GameObject*>(m_objects))
                    if (obj) lm = std::max(lm, obj->getPositionX());
            }
            static int s_cc = 0;
            if (++s_cc <= 40)
                writeResult("clearcheck: x=" + std::to_string((int)m_player1->getPositionX())
                    + " levelMaxX=" + std::to_string((int)lm)
                    + " goalX=" + std::to_string((int)solver::g_goalX)
                    + " margin=" + std::to_string((int)(lm - m_player1->getPositionX()))
                    + " tick=" + std::to_string(g_tick)
                    + " solve=0");
        }
        // Always report the coin count (makes the replay verification of spec §6.5 hold for
        // coins as well)
        if (g_started && g_cfg.coinMode && !solver::g_coins.empty()) {
            size_t got = 0;
            for (auto pu : solver::g_coinPickupTick)
                if (pu >= 0 && pu <= g_tick) ++got;
            // COARSE ON PURPOSE, and the line has to say so. This witness tests
            // |dx| and |dy| against COIN_RADIUS (20) at the coin's LOAD
            // position, while GD credits the player's per-mode box against the
            // coin's LIVE one -- 35 for a cube, 33.5 for a spider, and the
            // moved row for a coin its group carries. So it reports FEWER coins
            // than GD on the same run, and that is not a disagreement.
            // Measured 2026-09-20 on lv21: GD credited coin 0 at |dx|=31.8,
            // which this test cannot reach, and the line read 2/3 against GD's
            // 3/3 on a clear that was genuinely all three -- read as a conflict
            // for twenty minutes. The rule belongs in the search (dp cli.hpp)
            // and not in a second copy here, so the bound is printed instead
            // and a reader can compare like with like.
            std::string s = "coin: level complete with " + std::to_string(got) + "/"
                + std::to_string(solver::g_coins.size())
                + " coins (COARSE r=" + std::to_string((int)solver::COIN_RADIUS)
                + " at the load position -- GD's own count is the coingd: line)"
                + " (pickup ticks:";
            for (auto pu : solver::g_coinPickupTick) s += " " + std::to_string(pu);
            writeResult(s + ")");
            // ...and GD's own verdict on the same coins, from the pickupItem hook.
            // Two lines, deliberately not merged into one number: the mod's test is
            // a claim ABOUT GD, and a claim and its subject have to be readable
            // apart before they can be compared.
            size_t gdGot = 0;
            for (auto pu : solver::g_coinGdTick) if (pu >= 0) ++gdGot;
            std::string g = "coingd: level complete with " + std::to_string(gdGot) + "/"
                + std::to_string(solver::g_coins.size()) + " coins (pickup ticks:";
            for (auto pu : solver::g_coinGdTick) g += " " + std::to_string(pu);
            writeResult(g + ") unmatched=" + std::to_string(solver::g_coinGdUnmatched));
            // The comparison itself, so a run cannot look green while the two
            // witnesses disagree. A disagreement is a difference in the VERDICT
            // (collected or not); the tick offset between two agreeing witnesses is
            // reported as a number rather than judged against a tolerance nobody
            // has measured yet -- our test runs at the end of the tick and GD's
            // runs inside the collision pass, so some lag is expected and its size
            // is one of the things this run is here to find out.
            std::string d;
            long long maxLag = 0;
            for (size_t i = 0; i < solver::g_coins.size(); ++i) {
                const long long ours = i < solver::g_coinPickupTick.size()
                                           ? solver::g_coinPickupTick[i] : -1;
                const long long gd = i < solver::g_coinGdTick.size()
                                         ? solver::g_coinGdTick[i] : -1;
                if ((ours >= 0) != (gd >= 0)) {
                    d += " #" + std::to_string(i)
                       + "(uid" + std::to_string(solver::g_coins[i].uid)
                       + " ours=" + std::to_string(ours)
                       + " gd=" + std::to_string(gd) + ")";
                } else if (ours >= 0) {
                    maxLag = std::max(maxLag, ours > gd ? ours - gd : gd - ours);
                }
            }
            writeResult("coincmp:" + (d.empty() ? std::string(" agree") : d)
                        + " maxlag=" + std::to_string(maxLag));
        }
        // A solve session's FIRST clear is the loop's own verification replay reaching the end.
        // It is bookkeeping, not an ending: nobody watched it (the screen was off and the sound
        // was muted) and the session carries straight on into showing the solution properly.
        //
        // GD's completion path is therefore not run for it. Running it and then restarting the
        // level -- which is what an earlier version of this did -- leaves the end-screen
        // sequence (showCompleteText, showEndLayer) firing on a scheduler against a level that
        // has been rebuilt underneath it. Measured on lv1: the showing began, was interrupted
        // and restarted at t=193, the attempt counter went from 2 to 17, and destroyPlayer was
        // called 208 times with nothing ever dying.
        const bool takeOver = g_started && dpsolve::takesOverClear();
        if (!takeOver) {
            // Everything levelComplete would record -- percentage, best score, completion,
            // orbs, diamonds, the secret key, coin achievements -- is skipped by GD itself
            // while this guard is up (see NoRecordGuard).
            NoRecordGuard nr(this);
            PlayLayer::levelComplete();
        }
        restoreProgress();
        if (g_started) {
            ++g_finishedAttempts;
            // Every tick of a cleared run is a surviving position, so it is the most valuable
            // corridor sample
            clearance::observe(solver::g_log, (long long)solver::g_log.size(), false);
            // Print the wall time spent burning through the end-zone effects (this number is
            // the only way to tell whether it is fixed)
            if (g_endzoneBurn) {
                double sec = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - g_endzoneStart).count();
                writeResult("endzone: burned in " + std::to_string(sec) + "s");
                g_endzoneBurn = false;
            }
            float cx = m_player1 ? m_player1->getPositionX() : -1.f;
            // A plain replay never runs buildPois, so levelMaxX is uncomputed; compute it
            // here (once at completion, so it does not affect run time)
            float maxX = solver::g_levelMaxX;
            if (maxX <= 0 && m_objects) {
                for (auto* obj : CCArrayExt<GameObject*>(m_objects))
                    if (obj) maxX = std::max(maxX, obj->getPositionX());
            }
            // Also print the goal x: lets the verification harness judge "did it really
            // reach the goal" without a per-level tolerance table
            float goalX = solver::g_goalX;
            if (goalX <= 0.f && m_endPortal) goalX = m_endPortal->getPositionX();
            rollGroupTrace();
            cpflight::noteEnd(g_tick);
            writeResult("complete: attempt=" + std::to_string(g_attempt)
                + " step=" + std::to_string(m_currentStep)
                + " tick=" + std::to_string(g_tick)
                + " x=" + std::to_string((int)cx)
                + " levelMaxX=" + std::to_string((int)maxX)
                + " goalX=" + std::to_string((int)goalX)
                // pct = GD's own progress percentage (a level-independent yardstick; used
                // for false-clear detection)
                + " pct=" + std::to_string(this->getCurrentPercent()));
            // The no-death recording pass reached the end of the level. That is NOT a clear --
            // nothing survived it, dying was simply switched off -- so it must never be filed
            // as a solution, and the session goes back to solving with what it recorded.
            //
            // Asked of the ATTEMPT, not of the recorder: the recorder can already have retired on
            // its tick budget while this very pass ran on to the end (see g_recordAttempt). When
            // it has, there is nothing left to finish -- just refuse the clear.
            if (g_cfg.dpSolve && dpsolve::g_recordAttempt) {
                if (dpsolve::g_deepActive) dpsolve::finishDeepRecord("reached the end");
                else writeResult("dpsolve: the no-death pass reached the end after its recorder "
                                 "had already retired - not a clear");
                return;
            }
            // cfg cpflightprobe: the probe or its control reaching the end is kept or compared like a
            // death, never filed.
            if (g_cfg.dpSolve && cpflight::g_probeFlying) {
                writeResult(cpflight::probeCapture(g_tick, cx, "cleared"));
                g_paused = true;
                return;
            }
            if (g_cfg.dpSolve && cpflight::g_controlFlying) {
                writeResult(cpflight::controlEnd(g_tick, cx, "cleared"));
                g_paused = true;
                return;
            }
            // GD raised levelComplete a long way short of the goal. It really does that -- an end
            // trigger inside a sub-area, a teleport past the finish line -- and the comment above
            // the clearcheck line at the top of this function has said so for as long as the line
            // has existed. What was missing was anything that ACTED on it: g_clearMargin
            // documented itself as the tolerance of a false-clear guard and was read nowhere, so
            // onCleared() accepted whatever GD raised.
            //
            // Measured on a custom level (2026-08-24): levelComplete at x=2,458 of 19,570 --
            // GD's own getCurrentPercent() said 4.85% in the same breath -- which was filed as the
            // solution (10 inputs, 140 bytes, for a 19,570px level), flipped the badge to REPLAY
            // and "showed" it, dying seconds in. That saved file is what Replay mode loads next
            // time, so a false clear does not stay inside its own session.
            //
            // Refusing is the whole of it: the run stopped here without finishing, which is what
            // onDeath already means, so it goes down the same path a death does and the loop
            // re-anchors from where it stopped.
            const float goal = (goalX > 0.f) ? goalX : maxX;
            // ...and the x-distance test alone cannot tell a false clear from a level whose
            // ENDING RUNS BACKWARDS (g_clearMargin's own comment warned of exactly this).
            // lv22: the final cube section retreats from x=22,375 to 21,853, so a genuine
            // completion sits 2,232px "short of" the end portal at 24,085 -- and the first
            // plan ever to beat the level was refused here, filed as a death at 91.7%%, and
            // the loop went back to searching underneath the results screen. GD's own
            // percentage is the level-independent yardstick this guard already printed but
            // never read: the measured false clear said 4.85%% in the same breath, the real
            // one says 99.18%%. A completion past 95%% is a completion.
            // cfg dpendtrigclear (on): ...unless an End trigger (id 3600) fired in this attempt --
            // it finishes a level wherever it fires. SubZero 4002 spawns one from a touch box at
            // x=30,857 on the way back (83%), which is where the level ends (the user, 2026-09-24:
            // it climbs while turning back and forth); refused, every such clear was booked as a
            // death at the "wall" t=23,616. The fire itself is the condition, not the object's
            // presence in the level: the false clear this gate was written for (2026-08-24) was
            // never traced to its cause, so a level merely holding an End trigger proves nothing.
            const bool endTrigLevel = g_cfg.dpEndTrigClear && solver::g_endTriggerFiredTick >= 0;
            // ...and not on a slice (level_slice.hpp), whose percentage is not the level's: level
            // 22's copy read 90.73% at the x and tick where the level itself read 96.44%. A clear
            // of the copy is only a candidate -- its plan is flown on the level itself before
            // anything is filed, and this gate judges it there.
            if (g_cfg.dpSolve && !g_dpShowSolution && cx >= 0.f && goal > 1.f
                && levelslice::g_phase != levelslice::Sliced
                && levelslice::g_phase != levelslice::RefFlight
                && (goal - cx) > g_clearMargin
                && this->getCurrentPercent() < 95.0f && !endTrigLevel) {
                char fb[256];
                snprintf(fb, sizeof(fb),
                    "dpsolve: refusing a clear %.0fpx short of the goal (x=%.0f goal=%.0f "
                    "pct=%.2f, tolerance %.0f) - not a solution",
                    (double)(goal - cx), (double)cx, (double)goal,
                    this->getCurrentPercent(), (double)g_clearMargin);
                writeResult(fb);
                dpsolve::onDeath(solver::attemptStateTick(g_tick), cx);
                return;
            }
            // cfg `coinroute`: a clear that left a coin behind is not the answer that was asked
            // for. An attempt normally ends AT the coin (the miss request in hooks_gamelayer),
            // so this is the second gate, not the first -- and it is needed because the first
            // one can be refused: GD ignores a kill aimed at a player in the moving-zombie
            // state, and a recording pass is deliberately never asked. Refused exactly as a
            // false clear is, so the loop re-anchors instead of filing it.
            if (g_cfg.dpSolve && !g_dpShowSolution && g_cfg.coinRoute
                && !solver::g_coins.empty()) {
                size_t got = 0;
                for (auto pu : solver::g_coinGdTick) if (pu >= 0) ++got;
                if (got < solver::g_coins.size()) {
                    writeResult("dpsolve: refusing a clear with " + std::to_string(got) + "/"
                                + std::to_string(solver::g_coins.size())
                                + " coins - not an all-coins solution");
                    // cfg coinmisspost: filed at the missed coin instead of at the finish.
                    long long dT = solver::attemptStateTick(g_tick);
                    float dX = cx;
                    dpsolve::fileCoinMissPost(dT, dX);
                    dpsolve::onDeath(dT, dX);
                    return;
                }
            }
            // A checkpoint flight (cfg `dpcheck`) can be what reached the end while a search is
            // still out. That is a real clear of the plan GD just flew, so it is filed like any
            // other below; the search in flight is abandoned (dpsolve::ckClearedDuringJob).
            bool byFlight = false;
            if (g_cfg.dpSolve && g_cfg.dpCheck && !g_dpShowSolution && dpsolve::g_running.load())
                byFlight = dpsolve::ckClearedDuringJob();
            // A clear of a slice (level_slice.hpp) is not a solution yet: its plan goes on to be
            // flown on the level itself. Past the two gates above, so what goes over is a real
            // clear of the copy.
            if (g_cfg.dpSolve && !g_dpShowSolution
                && levelslice::onSliceCleared(g_cfg.inputs, g_tick))
                return;
            // cfg cpflight: a clear from a checkpoint is not filed. The plan flies once more from
            // the head (hooks_gamelayer, at the next frame boundary), and that flight's clear is.
            if (g_cfg.dpSolve && !g_dpShowSolution && cpflight::g_from >= 0) {
                writeResult("cpflight: attempt " + std::to_string(g_attempt)
                            + " cleared from the checkpoint at t=" + std::to_string(cpflight::g_from)
                            + " - flying the plan from the head before it is filed");
                cpflight::g_forceHead = true;
                cpflight::g_headPending = true;
                return;
            }
            if (g_cfg.dpSolve && !g_dpShowSolution) levelslice::onLevelCleared(g_tick);
            // A plan that has just been SEEN to clear the level is a solution; file it under
            // the name Replay mode looks for, so the next visit does not have to solve again.
            // Only here: a plan that has not cleared is not a solution, whatever else it is.
            if (g_cfg.dpSolve && !g_dpShowSolution && !g_cfg.inputs.empty()) {
                char name[128];
                snprintf(name, sizeof(name),
                         g_cfg.coinFiles ? "%s/solution_lv%d_coins.txt"
                                         : "%s/solution_lv%d_dp.txt",
                         DATA_DIR, g_cfg.levelId);
                if (writeInputsFile(name, g_cfg.inputs)) {
                    // ...and which version solved it, for the play menu (solvedWithPath).
                    writeSolvedWith(name, Mod::get()->getVersion().toVString());
                    writeResult(std::string("dpsolve: solution saved -> ") + name);
                    notify::show("gdsolver: solution saved", NotificationIcon::Success, 3.f);
                }
                // ...and next to it, the map of where the rounds went (itermap.hpp), so the
                // showing that follows -- and every later replay of this solution -- can draw
                // the run's own history over the level. Written on the clear, alongside the
                // solution, for the same reason: this is the moment both are true.
                // "itermap saved", never "iteration ...": see the note at the giveUp() copy.
                // The clearing round's own tail first -- no death puts it there.
                dpsolve::mapClear(g_tick, byFlight);
                if (itermap::save(g_cfg.levelId, true))
                    writeResult("dpsolve: itermap saved -> "
                                + itermap::pathFor(g_cfg.levelId));
            }
            // The first clear of a solve session ends the SOLVING, not the session: the run that
            // proved the plan was one of the loop's own verification replays, which are drawn
            // nowhere and heard by nobody. Hand over to the showing of it (dpsolve::poll) and
            // keep the session open; the second clear -- the one the player watched -- ends it
            // here in the ordinary way.
            if (g_cfg.dpSolve && dpsolve::onCleared()) return;
            // A finished panel replay is left to the original levelComplete's normal clear
            // handling (results screen)
            endSession("level_complete");
        }
    }
};
