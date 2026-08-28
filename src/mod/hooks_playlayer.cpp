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
        __try {
            PlayLayer::updateVisibility(dt);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            logVisibilityCrashSwallowed();
        }
    }
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        // Depending on the panel mode, auto-configure a session whichever level is entered.
        // Level selection can use the game's own UI as is (custom levels included).
        // A session in progress (g_started && !g_sessionOver) (including autorun.cfg
        // launches) is not interfered with. Leftovers of a finished session count as absent
        // (uiConfigureSession resets everything)
        bool want = (!g_started || g_sessionOver) && g_uiMode > 0 && level;
        if (want) uiConfigureSession(level->m_levelID.value());
        // Sample the level's own record before it has run a tick, so the session-end line can
        // state whether anything was written into it (see progressDiff).
        g_progressLevel = level;
        g_progressAtStart = sampleProgress(level);
        return PlayLayer::init(level, useReplay, dontCreateObjects);
    }

    void onQuit() {
        // Exiting from the pause menu = session aborted. endSession alone is not enough:
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
        PlayLayer::onQuit();
    }

    void resetLevel() {
        // A reset of an old layer (embers awaiting the scene swap) must not touch the books
        if (this != PlayLayer::get()) { PlayLayer::resetLevel(); restoreProgress(); return; }
        // A section-solver restore is "inside the search", so it must not touch the mod's
        // bookkeeping at all. Letting it through runs a grouptrace rebuild, POI rebuild and
        // retry logging on every restore, turning a restore that should take 2ms into tens
        // of ms and mixing up the recordings too.
        if (secsolve::g_active) { PlayLayer::resetLevel(); restoreProgress(); return; }
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
            g_nextToggle = 0;
            g_tick = 0;
            g_gameFrame = 0;   // rotation does not carry across attempts
            anchors::onAttemptStart();   // the re-anchor record is per attempt
            itermap::onAttemptStart();   // ...and so is the seek bar's tick -> x record
            solver::g_coinPickupTick.assign(solver::g_coins.size(), -1);
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
            // Required for coinMode / clearance: a plain replay never runs buildPois, and
            // with g_coins / g_solids empty neither pickup detection nor sample collection
            // ever runs
            if ((g_cfg.coinMode || clearance::g_on || g_cfg.dpSelfTest || g_cfg.dpSolve)
                && !solver::g_poisBuilt) {
                solver::buildPois(this);
                solver::g_poisBuilt = true;
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
        if (!ckptRestore && g_started && g_cfg.blockInput) {
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
        if (!grouptrace::g_on) return;
        auto r = grouptrace::roll();
        writeResult("gt_last: attempt=" + std::to_string(g_attempt)
            + " rows=" + std::to_string(r.rows)
            + " depth=" + std::to_string(r.depth));
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (this != PlayLayer::get()) { PlayLayer::destroyPlayer(player, object); return; }
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
        if ((g_cfg.hitboxTrace || g_cfg.dpSolve) && g_started && !g_sessionOver
            && player) {
            char kb[240];
            snprintf(kb, sizeof(kb),
                "killer: t=%lld who=%s py=%.3f pvy=%.3f px=%.3f obj=%s uid=%d "
                "id=%d type=%d ox=%.3f oy=%.3f",
                (long long)g_tick,
                player == m_player1 ? "p1" : player == m_player2 ? "p2" : "?",
                player->getPositionY(), (double)player->m_yVelocity,
                player->getPositionX(), object ? "yes" : "NULL",
                object ? (int)object->m_uniqueID : -1,
                object ? (int)object->m_objectID : -1,
                object ? (int)object->m_objectType : -1,
                object ? object->getPositionX() : 0.f,
                object ? object->getPositionY() : 0.f);
            writeResult(kb);
            // The same verdict, latched for the iteration map (itermap.hpp). GD holds the killer
            // in an argument at this instant and nowhere afterwards, and the recorder's last row
            // is a tick short of the death -- so this is the only place the mark's y and the
            // object that put it there can both be had.
            if (player == m_player1)
                itermap::latchKiller((long long)g_tick, player->getPositionY(),
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
        if (secsolve::g_noKill) {
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
        // Count which player a death in a dual section came from (diagnostic)
        bool wasDead = player->m_isDead;
        if (!wasDead && player) {
            if (player == m_player1) ++solver::g_deathsP1;
            else if (player == m_player2) ++solver::g_deathsP2;
            else ++solver::g_deathsOther;
        }
        // Direct evidence of what killed the player (only while measuring gatetrace)
        if (!wasDead && player == m_player1 && solver::g_gateTrace) {
            char kb[280];
            if (object) {
                // Emit BOTH the position and the hitbox. For an object moved by a group,
                // getPosition may stay at its entry value while only the rect moves (the
                // spike uid 13495 in lv20 has position 24,401 while its rect is at 24,842),
                // and looking at the position alone gives the unreadable story "killed by an
                // object 441px away"
                auto orr = object->getObjectRect();
                snprintf(kb, sizeof(kb),
                    "killer: tick=%lld uid=%d id=%d type=%d ox=%.1f oy=%.1f "
                    "rect=%.2f,%.2f,%.2f,%.2f on=%d px=%.1f py=%.1f",
                    (long long)g_tick, object->m_uniqueID, object->m_objectID,
                    (int)object->m_objectType,
                    object->getPositionX(), object->getPositionY(),
                    orr.origin.x, orr.origin.y, orr.size.width, orr.size.height,
                    (object->m_isGroupDisabled || object->m_isGroupDisabledTemp)
                        ? 0 : 1,
                    player->getPositionX(), player->getPositionY());
            }
            else
                snprintf(kb, sizeof(kb), "killer: tick=%lld (no object) px=%.1f py=%.1f",
                    (long long)g_tick,
                    player->getPositionX(), player->getPositionY());
            writeResult(kb);
        }
        {
            // GD records the level's percentage from inside destroyPlayer; the guard makes it
            // take its own no-record path (see NoRecordGuard).
            NoRecordGuard nr(this);
            PlayLayer::destroyPlayer(player, object);
        }
        restoreProgress();
        // In dual mode a p2 death also ends the run (restricting to p1, sections where only
        // p2 dies never get booked and the run spins idle). The position used is p1's: all
        // recordings are p1-based, and the two dual bodies share the same x (y is mirrored)
        bool isP1 = (player == m_player1);
        bool isP2Dual = (player == m_player2 && m_gameState.m_isDualMode);
        // Exclude anti-cheat pseudo-calls: record only on an actual transition into the
        // dead state
        if (!wasDead && player->m_isDead && (isP1 || isP2Dual)
            && !solver::g_deathBooked) {
            solver::g_deathBooked = true;
            auto pos = (isP1 || !m_player1) ? player->getPosition()
                                            : m_player1->getPosition();
            if (isP2Dual) {
                static int s_p2logs = 0;
                if (++s_p2logs <= 5)
                    writeResult("dual: booked a p2 death as the run end (tick="
                        + std::to_string((long long)g_tick)
                        + " x=" + std::to_string((int)pos.x) + ")");
            }
            ev("destroyPlayer", pos.x, pos.y);
            // Corridor clearance sample collection (cfg clearance=1). Accumulate the
            // "positions that survived" and "distances stopped by a surface" this run showed
            clearance::observe(solver::g_log, g_tick);
            if (g_started) {
                ++g_finishedAttempts;
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - g_attemptStart).count();
                double speed = ms > 0 ? (g_tick / 240.0) / (ms / 1000.0) : 0;
                // Finalise the recordings BEFORE `death:`: the reader learns the end of the
                // run from this line and reads dump.csv / grouptrace_last.txt right after
                rollGroupTrace();
                flushAll();
                writeResult("death: attempt=" + std::to_string(g_attempt)
                    + " tick=" + std::to_string(g_tick)
                    + " x=" + std::to_string(pos.x)
                    + " wallMs=" + std::to_string(ms)
                    + " speedX=" + std::to_string(speed));
                if (g_serveMode) g_serveWait = true;  // stop at the start of the next attempt
                // Stage C: the in-process loop treats this death as its next question -- where
                // is the model wrong, and what does GD say the state really was just before.
                // It freezes the level and re-solves the tail; the repaired plan is installed
                // at the next frame boundary (dpsolve::poll)
                if (g_cfg.dpSolve) dpsolve::onDeath(g_tick, pos.x);
            }
        }
    }

    void levelComplete() {
        if (this != PlayLayer::get()) {
            NoRecordGuard nr(this);
            PlayLayer::levelComplete();
            return;
        }
        hookdepth::Guard hg(hookdepth::COMPLETE);
        stallwatch::Mark sm(stallwatch::COMPLETE);
        ev("levelComplete");
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
            std::string s = "coin: level complete with " + std::to_string(got) + "/"
                + std::to_string(solver::g_coins.size()) + " coins (pickup ticks:";
            for (auto pu : solver::g_coinPickupTick) s += " " + std::to_string(pu);
            writeResult(s + ")");
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
            // GD raised levelComplete a long way short of the goal. It really does that -- an end
            // trigger inside a sub-area, a teleport past the finish line -- and the comment above
            // the clearcheck line at the top of this function has said so for as long as the line
            // has existed. What was missing was anything that ACTED on it: g_clearMargin
            // documented itself as the tolerance of a false-clear guard and was read nowhere, so
            // onCleared() accepted whatever GD raised.
            //
            // Measured on level 140155559 (2026-08-24): levelComplete at x=2,458 of 19,570 --
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
            if (g_cfg.dpSolve && !g_dpShowSolution && cx >= 0.f && goal > 1.f
                && (goal - cx) > g_clearMargin
                && this->getCurrentPercent() < 95.0f) {
                char fb[256];
                snprintf(fb, sizeof(fb),
                    "dpsolve: refusing a clear %.0fpx short of the goal (x=%.0f goal=%.0f "
                    "pct=%.2f, tolerance %.0f) - not a solution",
                    (double)(goal - cx), (double)cx, (double)goal,
                    this->getCurrentPercent(), (double)g_clearMargin);
                writeResult(fb);
                dpsolve::onDeath(g_tick, cx);
                return;
            }
            // A plan that has just been SEEN to clear the level is a solution; file it under
            // the name Replay mode looks for, so the next visit does not have to solve again.
            // Only here: a plan that has not cleared is not a solution, whatever else it is.
            if (g_cfg.dpSolve && !g_dpShowSolution && !g_cfg.inputs.empty()) {
                char name[128];
                snprintf(name, sizeof(name), "%s/solution_lv%d_dp.txt",
                         DATA_DIR, g_cfg.levelId);
                if (writeInputsFile(name, g_cfg.inputs)) {
                    writeResult(std::string("dpsolve: solution saved -> ") + name);
                    notify::show("gdsolver: solution saved", NotificationIcon::Success, 3.f);
                }
                // ...and next to it, the map of where the rounds went (itermap.hpp), so the
                // showing that follows -- and every later replay of this solution -- can draw
                // the run's own history over the level. Written on the clear, alongside the
                // solution, for the same reason: this is the moment both are true.
                // "itermap saved", never "iteration ...": see the note at the giveUp() copy.
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
