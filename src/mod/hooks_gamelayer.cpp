// GJBaseGameLayer hook: tick control, fast loop, replay/serve, section solver, tracing.
#include "mod/playlayer_helpers.hpp"

using namespace p1;

// ---- Tracing: frame / step structure ----
class $modify(GJBaseGameLayer) {
    // The EXACT rect the spider's target search queries (cfg `hitboxtrace=1`,
    // `srect:`/`drect:` lines). spiderTestJumpInternal (win 0x3943f0) builds two
    // rects -- solids and damaging -- whose transverse anchoring in a rotated
    // frame defeated every closed-form fit (lv22 t=1,813: GD's candidate list
    // holds uid1204 at a 6px GAP from the player's box; the injection sweep
    // brackets the cutoff between centre distances 37.5 and 41.5). Printing the
    // rect the binary actually built is the instrument that replaces the
    // guessing; these hooks fire from every caller, so the tick stamp and the
    // hitboxtrace gate keep it to the probe runs that ask for it.
    cocos2d::CCArray* staticObjectsInRect(cocos2d::CCRect rect, bool enabledGroups) {
        // The tick window, not a line cap: this query runs constantly (4,000
        // lines burnt in the first second of the natural attempt), so a cap
        // never survives to the tick under study.
        if (g_cfg.hitboxTrace && g_started && !g_sessionOver
            && g_tick >= 1750 && g_tick <= 1900) {
            {
                char b[192];
                snprintf(b, sizeof(b),
                         "srect: t=%lld o=(%.3f,%.3f) s=(%.3f,%.3f) eg=%d",
                         (long long)g_tick, rect.origin.x, rect.origin.y,
                         rect.size.width, rect.size.height, (int)enabledGroups);
                writeResult(b);
            }
        }
        return GJBaseGameLayer::staticObjectsInRect(rect, enabledGroups);
    }
    cocos2d::CCArray* damagingObjectsInRect(cocos2d::CCRect rect, bool enabledGroups) {
        if (g_cfg.hitboxTrace && g_started && !g_sessionOver
            && g_tick >= 1750 && g_tick <= 1900) {
            {
                char b[192];
                snprintf(b, sizeof(b),
                         "drect: t=%lld o=(%.3f,%.3f) s=(%.3f,%.3f) eg=%d",
                         (long long)g_tick, rect.origin.x, rect.origin.y,
                         rect.size.width, rect.size.height, (int)enabledGroups);
                writeResult(b);
            }
        }
        return GJBaseGameLayer::damagingObjectsInRect(rect, enabledGroups);
    }
    // When and by which object the rotate trigger fired (cfg `hitboxtrace=1`, `rot:` line).
    //
    // The model's crossing test runs in the progress coordinate of the rotation frame,
    // and in frame 3 that becomes "the trigger's world Y", so uid5957 (8297,423),
    // 1,500px away in world space, misfires at u=423 (lv22 t=4,688).
    // Replacing it with "cross in world X" broke the first section, so
    // do not guess the test axis -- ask GD. Once this line appears, the tick and the
    // partner object are settled.
    void rotateGameplay(RotateGameplayGameObject* object) {
        // Absolute angle. Same convention as the model's nf (lround(rot/90) & 3).
        if (object)
            g_gameFrame = ((int)std::lround(object->getRotation() / 90.0)) & 3;
        if (g_cfg.hitboxTrace && g_started && !g_sessionOver) {
            static int lines = 0;
            if (++lines <= 200) {
                auto p = object ? object->getPosition() : cocos2d::CCPoint(0, 0);
                // Also emit properties that objrects does not export. Some objects with
                // the same orot=0 reverse and some do not, yet the exported rows were
                // completely identical and could not be told apart (lv22 uid 16659 and
                // 1215). The distinguishing bit must be in one of these four.
                char b[256];
                snprintf(b, sizeof(b),
                         "rot: t=%lld uid=%d id=%d obj=(%.1f,%.1f) orot=%.1f "
                         "px=%.3f py=%.3f isRev=%d endRev=%d mvDir=%d gndDir=%d",
                         (long long)g_tick,
                         object ? object->m_uniqueID : -1,
                         object ? object->m_objectID : -1,
                         p.x, p.y, object ? object->getRotation() : 0.f,
                         m_player1 ? m_player1->getPositionX() : 0.f,
                         m_player1 ? m_player1->getPositionY() : 0.f,
                         object ? (int)object->m_isReverse : -1,
                         object ? (int)object->m_endReversed : -1,
                         object ? (int)object->m_moveDirection : -1,
                         object ? (int)object->m_groundDirection : -1);
                writeResult(b);
            }
        }
        GJBaseGameLayer::rotateGameplay(object);
    }

    // When and by which object the Options trigger (id 2899) fired
    // (cfg `hitboxtrace=1`, `opt:` line).
    //
    // Why it is needed: in lv22, m_controlsDisabled is set in 4 sections, and buttons
    // are ignored entirely while it is. Trying to reverse-engineer the firing rule from
    // the dump's transitions failed because the same x is crossed repeatedly (reverse
    // travel), so it could not be decided whether it is one-shot or per-crossing, or
    // whether height matters. Having GD name the partner uid settles it in one shot.
    void processOptionsTrigger(GameOptionsTrigger* object) {
        if (g_cfg.hitboxTrace && g_started && !g_sessionOver) {
            static int lines = 0;
            if (++lines <= 200) {
                auto p = object ? object->getPosition() : cocos2d::CCPoint(0, 0);
                char b[256];
                snprintf(b, sizeof(b),
                         "opt: t=%lld uid=%d id=%d obj=(%.1f,%.1f) "
                         "px=%.3f py=%.3f p1=%d p2=%d touch=%d spawn=%d "
                         "multi=%d was=%d",
                         (long long)g_tick,
                         object ? object->m_uniqueID : -1,
                         object ? object->m_objectID : -1,
                         p.x, p.y,
                         m_player1 ? m_player1->getPositionX() : 0.f,
                         m_player1 ? m_player1->getPositionY() : 0.f,
                         object ? (int)object->m_disableP1Controls : -9,
                         object ? (int)object->m_disableP2Controls : -9,
                         object ? (int)object->m_isTouchTriggered : -1,
                         object ? (int)object->m_isSpawnTriggered : -1,
                         object ? (int)object->m_isMultiTriggered : -1,
                         m_player1 ? (int)m_player1->m_controlsDisabled : -1);
                writeResult(b);
            }
        }
        GJBaseGameLayer::processOptionsTrigger(object);
    }

    // When, by whom, and in which direction a group toggle happened
    // (cfg `hitboxtrace=1`, `togl:` line).
    //
    // Why it is needed: lv22's teleport exit cluster (group 309: ship portal uid 6440
    // etc.) is controlled by two Toggle (id 1049) triggers; GD did not fire it even
    // though the player sat inside the portal rect for 13 ticks, and fired it on the
    // tick after the x=15,765 crossing (t=14,226). There are 3 crossings
    // (forward -> reverse -> forward), and "which crossing counted" is the same unsolved
    // gate as 2899 -- do not guess; have GD name it together with triggerID (the uid
    // of the firing object).
    void toggleGroupTriggered(int group, bool activate,
                              gd::vector<int> const& remapKeys,
                              int triggerID, int controlID) {
        if (g_cfg.hitboxTrace && g_started && !g_sessionOver) {
            static int lines = 0;
            if (++lines <= 2000) {
                char b[192];
                snprintf(b, sizeof(b),
                         "togl: t=%lld g=%d on=%d trig=%d ctl=%d px=%.3f",
                         (long long)g_tick, group, activate ? 1 : 0,
                         triggerID, controlID,
                         m_player1 ? m_player1->getPositionX() : 0.f);
                writeResult(b);
            }
        }
        GJBaseGameLayer::toggleGroupTriggered(group, activate, remapKeys,
                                              triggerID, controlID);
    }

    // Likewise, the direct call of the group toggle (`toglD:` line).
    // Callers that do not go through toggleGroupTriggered (collision blocks etc.) show up
    // only here -- lv22's group 309 never appeared on the Triggered side, so both are
    // hooked to pin down the real activation path.
    void toggleGroup(int id, bool activate) {
        if (g_cfg.hitboxTrace && g_started && !g_sessionOver) {
            static int lines = 0;
            if (++lines <= 2000) {
                char b[128];
                snprintf(b, sizeof(b), "toglD: t=%lld g=%d on=%d px=%.3f",
                         (long long)g_tick, id, activate ? 1 : 0,
                         m_player1 ? m_player1->getPositionX() : 0.f);
                writeResult(b);
            }
        }
        GJBaseGameLayer::toggleGroup(id, activate);
    }

    // Watch the collision candidate list (cfg `watchuid=N` + hbfrom/hbto, `ccl:` line).
    // Answers in one bit the last remaining suspect for "the test formula is right but it
    // does not fire" = "it is not in the list in the first place". has = whether watchuid
    // is in the list.
    void collisionCheckObjects(PlayerObject* player,
                               gd::vector<GameObject*>* objects,
                               int objectCount, float dt) {
        if (g_cfg.watchUid > 0 && g_started && !g_sessionOver && objects
            && player == m_player1
            && g_tick >= g_cfg.hbFrom
            && (g_cfg.hbTo == 0 || g_tick <= g_cfg.hbTo)) {
            static int lines = 0;
            if (++lines <= 2000) {
                int has = 0;
                const int n = (int)objects->size();
                const int lim = objectCount < n ? objectCount : n;
                for (int i = 0; i < lim; ++i) {
                    GameObject* o = (*objects)[i];
                    if (o && o->m_uniqueID == g_cfg.watchUid) { has = 1; break; }
                }
                const auto prct = player->getObjectRect();
                char b[224];
                snprintf(b, sizeof(b),
                         "ccl: t=%lld n=%d has=%d "
                         "prect=(%.3f,%.3f,%.3f,%.3f)",
                         (long long)g_tick, lim, has,
                         prct.origin.x, prct.origin.y,
                         prct.size.width, prct.size.height);
                writeResult(b);
            }
        }
        GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);
    }

    // When and by whom gravity was flipped (cfg `hitboxtrace=1`, `fg:` line).
    //
    // Why it is needed: at lv22 t=2,749 GD set upsideDown 0->1, and the model misread
    // that as "hit the ceiling and stopped". But no causing object could be found -- the
    // level has no id 2066 (the 2.2 gravity trigger), and `m_gravityValue` is the
    // default 1.0 on every EffectGameObject, so triggers.txt cannot tell. Watching the
    // moment of the call is the reliable way (the sign of the player's rotation also
    // could not be closed by sweep + static reading, and the hook settled it at once).
    void flipGravity(PlayerObject* p, bool flip, bool noEffects) {
        const bool watch = g_cfg.hitboxTrace && g_started && !g_sessionOver
                           && p && p == m_player1;
        const int before = watch ? (int)p->m_isUpsideDown : 0;
        GJBaseGameLayer::flipGravity(p, flip, noEffects);
        if (!watch) return;
        static int lines = 0;
        if (++lines > 2000) return;
        char b[192];
        snprintf(b, sizeof(b),
                 "fg: t=%lld flip=%d noFx=%d up %d->%d x=%.3f y=%.3f vy=%.3f",
                 (long long)g_tick, flip ? 1 : 0, noEffects ? 1 : 0,
                 before, (int)p->m_isUpsideDown,
                 p->getPositionX(), p->getPositionY(), p->m_yVelocity);
        writeResult(b);
    }

    void update(float dt) {
        // Re-entrancy guard. GD's resetLevel calls update internally, so every hook
        // re-entry stacks another fast loop and the stack runs out (0xC00000FD). Nested
        // calls pass through; session bookkeeping, the fast loop and hotkeys belong to
        // the outermost call only
        if (hookdepth::g_slots[hookdepth::BGL_UPDATE].cur > 0) {
            ++g_reentrantUpdates;
            GJBaseGameLayer::update(dt);
            return;
        }
        hookdepth::Guard hg(hookdepth::BGL_UPDATE);
        stallwatch::Mark sm(stallwatch::UPDATE);
        ++g_frame;
        // Verification C: fake the frame dt. This dt is the only path by which the physics
        // learns the frame rate.
        if (g_started && g_cfg.framedt > 0) dt = g_cfg.framedt;
        // Slow motion (F11 / cfg slowmo). The substep size stays 1/240; only "how many
        // substeps per frame" shrinks. Trajectory and tick numbers are unchanged
        if (g_slowmo > 1.f) dt /= g_slowmo;
        // Only the slow side of spectating speed (0.25x/0.5x) is made by dividing dt here.
        // The fast side is made by "update several times with the same dt" (loops below)
        // -- doubling dt would shift the position of processing tied to frame boundaries.
        // Either way the 1/240 step is unchanged
        if (g_watchSpeed < 1.f && !(g_cfg.fastdt > 0 && !g_realtimeOverride))
            dt *= g_watchSpeed;
        // The command file belongs to the driver and is only read while the session is live.
        // The KEYS outlive the session on purpose: clearing the level ends it, and F7 (replay
        // again) / F8 (rendering) / F9 (leave) are exactly the keys a person reaches for at
        // that moment
        if (g_started) {
            if (!g_sessionOver) pollCommandFile();
            pollHotkeys();
        }
        // Show the role in the window title (prevents mixing up a human's GD with a
        // worker). SetWindowText is only called when the content changes, so calling it
        // every frame is fine
        {
            static int s_titleTick = 0;
            if ((++s_titleTick & 63) == 0) updateWindowTitle();
        }
        // Simulated frame drops (cfg `lagms`). For reproducing a heavy environment.
        // With `lagat` it becomes "stall once, for a long time, at that tick"
        if (g_lagMs > 0 && g_started && !g_sessionOver) {
            if (g_lagAtTick < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(g_lagMs));
            } else if (!g_lagFired && g_tick >= g_lagAtTick) {
                g_lagFired = true;
                writeResult("lag: stalling " + std::to_string(g_lagMs)
                    + "ms at tick=" + std::to_string((long long)g_tick));
                std::this_thread::sleep_for(std::chrono::milliseconds(g_lagMs));
            }
        }
        // Always record a frame whose real time was too long (under syncprobe).
        // Sampling every 0.5 s alone misses the frame that caused it
        if (g_syncProbe && g_started && !g_sessionOver) {
            static std::chrono::steady_clock::time_point s_prevFrame{};
            auto nowFrame = std::chrono::steady_clock::now();
            if (s_prevFrame.time_since_epoch().count() != 0) {
                double frameMs = std::chrono::duration<double, std::milli>(
                    nowFrame - s_prevFrame).count();
                if (frameMs > 100.0) {
                    double musicSec = 0.0;
                    if (auto* fe = FMODAudioEngine::sharedEngine())
                        musicSec = fe->getMusicTimeMS(0) / 1000.0;
                    char hb[192];
                    snprintf(hb, sizeof(hb),
                        "hitch: frame=%.0fms tick=%lld game=%.2f music=%.2f d=%.2f",
                        frameMs, (long long)g_tick, (double)g_tick / 240.0, musicSec,
                        musicSec - (double)g_tick / 240.0);
                    writeResult(hb);
                }
            }
            s_prevFrame = nowFrame;
        }
        // Measure the drift between music and game time (cfg `syncprobe=1`). To tell who
        // drifted, line up real time / game time (tick/240) / GD's own attemptTime / the
        // FMOD music position
        if (g_syncProbe && g_started && !g_sessionOver) {
            static std::chrono::steady_clock::time_point s_lastSync{};
            auto nowSync = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(nowSync - s_lastSync).count() >= 0.5) {
                s_lastSync = nowSync;
                auto* pl = PlayLayer::get();
                double musicSec = 0.0;
                if (auto* fe = FMODAudioEngine::sharedEngine())
                    musicSec = fe->getMusicTimeMS(0) / 1000.0;
                double gameSec = (double)g_tick / 240.0;
                // timeForPos returns, from the level's speed layout, "the time at which
                // this position should be reached" (GD uses it for music/trigger sync).
                // Difference from the real game time = fast/slow relative to the chart
                double wantSec = -1.0;
                if (m_player1) wantSec = (double)this->timeForPos(
                    m_player1->getPosition(), 0, 0, true, 0);
                char sb[352];
                snprintf(sb, sizeof(sb),
                    "sync: real=%.2f game=%.2f attempt=%.2f music=%.2f "
                    "d(music-game)=%.2f want=%.2f d(game-want)=%.2f "
                    "spd=%.2f tick=%lld x=%.0f locked=%d endAnim=%d",
                    std::chrono::duration<double>(nowSync - solver::g_solveStart).count(),
                    gameSec, pl ? pl->m_attemptTime : -1.0, musicSec, musicSec - gameSec,
                    wantSec, gameSec - wantSec,
                    m_player1 ? (float)m_player1->m_playerSpeed : -1.f,
                    (long long)g_tick,
                    m_player1 ? m_player1->getPositionX() : -1.f,
                    m_player1 ? (int)m_player1->m_isLocked : -1,
                    pl ? (int)pl->m_levelEndAnimationStarted : -1);
                writeResult(sb);
            }
        }
        // Rollback verification (method B): run practice/checkpoint/restore at a frame
        // boundary
        if (g_started && !g_sessionOver) {
            auto* pl = PlayLayer::get();
            if (pl) {
                if (g_cfg.practiceAt >= 0 && !g_practiceOn && g_tick >= g_cfg.practiceAt) {
                    g_practiceOn = true;
                    pl->togglePracticeMode(true);
                    ev("PRACTICE_on");
                    writeResult("practice_on: tick=" + std::to_string(g_tick));
                }
                if (g_cfg.checkpointAt >= 0 && !g_ckpt && g_tick >= g_cfg.checkpointAt) {
                    g_ckpt = pl->markCheckpoint();
                    if (g_ckpt) {
                        g_ckpt->retain();
                        g_ckptTick = g_tick;
                        g_headHeld = 0;
                        if (m_player1) {
                            auto it = m_player1->m_holdingButtons.find(1);
                            g_headHeld = (it != m_player1->m_holdingButtons.end()
                                          && it->second) ? 1 : 0;
                        }
                        ev("CHECKPOINT_made", (double)g_ckptTick);
                        // Also emit x/y: without matching against the post-restore position
                        // we would miss that "the point where the checkpoint was made" and
                        // "the point the restore returns to" differ
                        char cb[160];
                        snprintf(cb, sizeof(cb),
                                 "checkpoint: tick=%lld x=%.3f y=%.3f",
                                 (long long)g_ckptTick,
                                 m_player1 ? m_player1->getPositionX() : -1.0,
                                 m_player1 ? m_player1->getPositionY() : -1.0);
                        writeResult(cb);
                    } else {
                        writeResult("checkpoint FAILED at tick=" + std::to_string(g_tick));
                    }
                }
                // Feasibility of the section-limited GD solver: run only restores N times
                // and measure the real cost per restore (cfg `restoreloop=N`). This is not
                // the search itself, so a broken state does not matter -- the session is
                // closed right after.
                if (g_cfg.restoreLoop > 0 && g_ckpt && !g_restoreLoopDone) {
                    g_restoreLoopDone = true;
                    const auto rt0 = std::chrono::steady_clock::now();
                    for (int i = 0; i < g_cfg.restoreLoop; ++i) {
                        if (pl->m_checkpointArray)
                            pl->m_checkpointArray->removeAllObjects();
                        pl->storeCheckpoint(g_ckpt);
                        g_restorePending = true;
                        pl->resetLevel();
                        g_restorePending = false;
                    }
                    const double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - rt0).count();
                    char rb[192];
                    snprintf(rb, sizeof(rb),
                             "restoreloop: n=%d totalMs=%.1f perMs=%.4f "
                             "perSec=%.0f ckptTick=%lld",
                             g_cfg.restoreLoop, ms, ms / g_cfg.restoreLoop,
                             1000.0 * g_cfg.restoreLoop / (ms > 0 ? ms : 1e-9),
                             (long long)g_ckptTick);
                    writeResult(rb);
                }
                // Section-limited GD solver. If the checkpoint exists and it has not run
                // yet, run it to completion inside this frame (no return to rendering
                // during the search).
                if (secsolve::g_on && g_ckpt && !secsolve::g_done
                    && g_tick >= secsolve::g_startTick) {
                    secsolve::g_done = true;
                    if (secsolve::g_worldDiff > 0) runWorldDiff(pl);
                    else if (secsolve::g_snapCmp > 0) runSnapCompare(pl);
                    else if (secsolve::g_verify) runSectionVerify(pl);
                    // The search is created suspended and takes its first slice below, on this
                    // same frame -- so the one-frame path and the sliced path start identically.
                    else {
                        secsolve::g_task = runSectionSolve(pl);
                        secsolve::g_taskLayer = pl;
                    }
                }
                if (g_cfg.restoreAt >= 0 && g_ckpt && !g_restoreDone && g_tick >= g_cfg.restoreAt) {
                    g_restoreDone = true;
                    long long fromTick = g_tick;
                    // The official restore path: reduce the list to our own checkpoint only,
                    // then resetLevel (in practice mode resetLevel restores to the latest
                    // checkpoint)
                    if (pl->m_checkpointArray) pl->m_checkpointArray->removeAllObjects();
                    pl->storeCheckpoint(g_ckpt);
                    g_restorePending = true;
                    pl->resetLevel();
                    g_restorePending = false;
                    ev("RESTORE_done", (double)fromTick, (double)g_ckptTick);
                    writeResult("restore: from=" + std::to_string(fromTick)
                        + " to=" + std::to_string(g_ckptTick) + " tickNow=" + std::to_string(g_tick));
                }
            }
        }
        // Pause/step for manual experiments (F2/F3/F4). Keys and the overlay are handled
        // here first so they work outside a session too (if a later return stopped the
        // keys from being picked up, the pause could never be released)
        updateOverlays(this);
        audio::sync();
        notify::watchdog();   // un-freeze and hide a notification a purge left pinned
        notify::selfTest();   // cfg `notifytest=1` only
        hitbox::perFrame(this);
        // The stall guard's deferred reset (see g_stallResetPending): performed here, at
        // the same boundary where plans are installed, so the whole next batch runs on the
        // fresh attempt from its first tick.
        // Both this and the poll below can reset the level, and a reset in the middle of a
        // section search replaces the world that search is expanding into -- so both wait for
        // it. Nothing is lost by waiting: a search ends its own session.
        if (g_stallResetPending && !secsolve::inFlight()) {
            g_stallResetPending = false;
            if (auto* pl = PlayLayer::get()) {
                writeResult("stall: resetLevel at the frame boundary");
                pl->resetLevel();
            }
        }
        if (!secsolve::inFlight())
            dpsolve::poll();   // installs the in-process solver's plan at a frame boundary
        pollProbeHotkeys();
        handleProbeRequest(this);
        // ---- one slice of the section search, then the frame goes back to the game ----
        // Placed after the overlays and the audio so each slice is bracketed by a HUD that
        // redraws and a song that stays stopped, and BEFORE every path that advances game time:
        // while a search is in flight the world belongs to it, exactly as it belongs to the DP
        // solve during its own freeze a few lines down.
        if (secsolve::inFlight()) {
            if (PlayLayer::get() != secsolve::g_taskLayer) {
                // The level was rebuilt underneath a suspended search. Its frame still points at
                // the old one, so it can only be dropped -- resuming would walk freed objects.
                writeResult("secsolve: the level was rebuilt during the search - dropping it");
                secsolve::g_task.destroy();
                secsolve::g_taskLayer = nullptr;
                secsolve::g_active = false;
                secsolve::g_noKill = false;
            } else {
                secsolve::g_sliceStart = std::chrono::steady_clock::now();
                secsolve::g_task.resume();
                if (secsolve::g_task.done()) {
                    secsolve::g_task.destroy();
                    secsolve::g_taskLayer = nullptr;
                }
                return;
            }
        }
        // Consume a render-resume request at the frame boundary: reset the level and
        // rebuild the render state with it. The order -- reset first, then resume
        // rendering -- is the point; in the reverse order the first updateVisibility after
        // resuming steps on state that piled up while stopped
        //
        // ABOVE THE STOPS, and that is the whole reason it is here rather than below them.
        // A search holds the level with g_paused (repair.hpp's spawn), so for the entire time the
        // solver thread is working, every frame returned before reaching this -- the request sat
        // raised, the screen stayed black, and pressing the key again did nothing because the
        // request it wanted to raise was already up. Reported 2026-08-28 as "F5 sometimes does not
        // switch": it always switched, just not until the search let a frame through. The reset is
        // safe here for the same reason the freeze is: the solver thread reads the recorder's
        // finished buffers, never the level.
        if (g_visResetPending && g_started && !g_sessionOver) {
            g_visResetPending = false;
            // The spectating speed is deliberately left alone: F8 is a render toggle now, and
            // forcing 1x here would silently undo the arrow keys. The cfg-driven paths
            // (watchat / watchback) set the speed themselves before asking for the reset.
            if (auto* pl = PlayLayer::get()) {
                log::info("watch: reset level, then resume rendering");
                pl->resetLevel();
                g_realtimeOverride = true;
                return;   // this frame ends here
            }
            g_realtimeOverride = true;
        }
        // Update the HUD before the pause branch (keep HUD and liveness display alive
        // while paused)
        // A seek in flight lifts the stop for as long as it takes to land, and the arrival puts
        // it back (g_seekRepause). It has to: a seek gets where it is going by running the level,
        // and with time frozen it never arrives -- a forward seek asked for while F2 was down
        // used to hang the level behind its own blackout for good. The one that needs this is a
        // backward frame-step, which physics cannot do in reverse and so must replay.
        if (probe::g_pause && !itermap::seeking()) {
            if (probe::g_step > 0) {
                GJBaseGameLayer::update(probe::g_step / 240.0f);
                probe::g_step = 0;
            }
            return; // freeze game time completely
        }
        // Auto-stop just before a wall (cfg `pauseatx`). Meant for freezing the screen to
        // take a picture; it only stops, so physics is unaffected (update is not called
        // afterwards = time does not advance)
        if (g_pauseAtX > 0.f && !g_pauseAtXFired && !g_paused && g_started
            && !g_sessionOver
            && m_player1 && m_player1->getPositionX() >= g_pauseAtX) {
            g_pauseAtXFired = true;   // once only; otherwise F7 could not resume
            g_paused = true;
            writeResult("pauseatx: paused at x=" + std::to_string(m_player1->getPositionX())
                + " y=" + std::to_string(m_player1->getPositionY())
                + " tick=" + std::to_string((long long)g_tick));
        }
        // While paused: do not call update at all, freezing game time. If a step was
        // requested, advance by exactly that much
        if (g_started && g_paused && !itermap::seeking()) {   // ...and the same for this stop
            if (g_stepTicks > 0) {
                ev("STEP_exec", g_stepTicks);
                GJBaseGameLayer::update(g_stepTicks / 240.0f);
                g_stepTicks = 0;
                flushAll();
            }
            return;
        }
        // A seek asked for by clicking the iteration map (itermap.hpp), when the target is
        // BEHIND the player and the level has to run again. A frame-boundary job, like every
        // other level change in this file -- resetLevel() from inside touch handling would be
        // the mistake the stall guard's deferred reset exists to avoid.
        if (itermap::g_seekRestart && g_started) {
            itermap::g_seekRestart = false;
            // The bar keeps working on the results screen, which is exactly when you want to go
            // back and look at something. Reviving is the same three lines F7 uses, and for the
            // same reason: the session ended with the level, not with the watching of it. Replays
            // only -- in serve mode the session belongs to the driver.
            if (itermap::g_seekRevive) {
                itermap::g_seekRevive = false;
                if (g_sessionOver && !g_serveMode) {
                    g_sessionOver = false;
                    g_finishedAttempts = 0;
                    ++g_cfg.maxAttempts;
                    writeResult("session_resume: the seek bar restarted the replay");
                }
            }
            g_paused = false;
            clearManualPause();
            g_pauseAtXFired = false;
            if (!g_sessionOver) {
                if (auto* pl = PlayLayer::get()) {
                    itermap::dismissEndScreen();   // or every clear stacks another one
                    // Before the reset, not after: resetLevel starts the song from the top, and
                    // audio::sync only gets to pause it on the NEXT frame -- which is the blip of
                    // the track's first moment you hear on every rewind.
                    audio::muteForSeek();
                    pl->resetLevel();
                    // ...and again on the OTHER side of it. The reset starts the song on a
                    // channel it creates itself, so the group silenced a line ago is not the
                    // group that is now playing (see hushNow).
                    audio::hushNow();
                    return;   // this frame ends here; the seek runs from the next one
                }
            }
            itermap::endSeek("nothing to restart");
        }
        // ...and the arrival. The seek ends where it was asked to end, or on a death that got
        // there first -- a plan that dies on the way is the answer to "what is at that point", so
        // the seek hands the speed back and lets it be seen rather than racing past it.
        if (itermap::seeking()) {
            const float sx = m_player1 ? m_player1->getPositionX() : 0.f;
            const bool sdead = !m_player1 || m_player1->m_isDead;
            const bool over = g_sessionOver;
            if (over) itermap::endSeek("the session ended");
            else if (itermap::seekArrived(sdead, dt))
                itermap::endSeek(sdead ? "died on the way" : "arrived");
            // Landed: put back the stop the seek had to lift to get here (a frame-step
            // backwards while paused is the case -- see g_seekRepause).
            if (!itermap::seeking() && itermap::g_seekRepause) {
                itermap::g_seekRepause = false;
                probe::g_pause = true;
                // A backward step aimed short on purpose; walk the last few ticks with the
                // step mechanism, which is exact where a seek batch is not. The pause branch
                // above consumes this on the next frame.
                long long fin = 0;
                if (itermap::g_seekStepTo >= 0)
                    fin = itermap::g_seekStepTo - itermap::g_nowTick;
                probe::g_step = (fin > 0 && fin < 240) ? (int)fin : 0;
                // The whole arithmetic of a backward step, in one line, because from outside the
                // process a step that lands in the wrong place and one that never ran look the
                // same. Reported 2026-08-28: a step back of 10 moved the tick FORWARD by 40.
                if (itermap::g_seekStepTo >= 0) {
                    char sb[192];
                    snprintf(sb, sizeof(sb),
                             "itermap: [stepback] landed t=%lld, wanted t=%lld, finishing +%d",
                             (long long)itermap::g_nowTick,
                             (long long)itermap::g_seekStepTo, probe::g_step);
                    writeResult(sb);
                }
                itermap::g_seekStepTo = -1;
            }
            // A rewind restarts the level, and GD's spawn effect is born behind the cover. Its
            // animation is driven by cocos ACTIONS, which only advance on rendered frames, and a
            // seek renders very few -- so the effect is still near its beginning when the cover
            // lifts and plays out over the arrival.
            //
            // NOT purgeDanglingActions() here, whatever the render toggle does. Tried
            // 2026-08-28: it takes the PLAYER's actions with everything else, and the player
            // came back from a rewind invisible -- its spawn animation is an action like any
            // other, and purging one mid-flight leaves the sprite wherever that action had got
            // to. The toggle can afford it because it resets the level immediately afterwards;
            // an arrival cannot.
            //
            // The circle waves are safe (they are effects that remove themselves on the
            // scheduler, which is why the sweep exists) but do not appear to be what is seen.
            if (!itermap::seeking() && g_fxSweep) sweepCircleWaves();
        }
        ev("BGL_update_pre", dt);
        // cfg `watchat`: at the given tick, cause the same state transition as F8 (return to
        // realtime). Must be the same transition as the F8 handler (a different path would
        // measure something different)
        bool wantWatch = (g_watchAtTick > 0 && g_tick >= g_watchAtTick)
            || (g_watchAtX > 0.f && m_player1 && m_player1->getPositionX() >= g_watchAtX);
        if (!wantWatch && g_watchAfterSec > 0.0) {
            double el = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - solver::g_solveStart).count();
            if (el >= g_watchAfterSec) wantWatch = true;
        }
        if (wantWatch && !g_realtimeOverride && g_cfg.fastdt > 0
            && g_watchStartTick < 0) {
            g_realtimeOverride = true; watchSpeedSet(WATCH_SPEED_1X);
            g_visRefresh = true;
            g_visResetPending = true;
            if (g_watchPurge) purgeDanglingActions();
            g_watchStartTick = g_tick;
            float wx = m_player1 ? m_player1->getPositionX() : -1.f;
            log::info("watchat: tick={} x={} -> realtime (F8 equivalent)",
                (long long)g_tick, wx);
            writeResult("watchat: tick=" + std::to_string((long long)g_tick)
                + " x=" + std::to_string(wx) + " -> realtime (F8 equivalent)");
            writeResult(fxCensus("watchat"));
        }
        // Reproduce the usage of pressing F8 repeatedly (cfg `watchcycle=<seconds>`).
        // Repeat the same state transition as the F8 handler on the wall clock
        if (g_watchCycleSec > 0.0 && g_cfg.fastdt > 0 && g_started && !g_sessionOver) {
            double el = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - solver::g_solveStart).count();
            if (g_watchCycleLast < 0.0) g_watchCycleLast = el;
            if (el - g_watchCycleLast >= g_watchCycleSec) {
                g_watchCycleLast = el;
                ++g_watchFlips;
                // Go through the same function as a human key press (poking the state
                // directly would measure something different)
                renderTogglePress();
                writeResult("watchcycle: press=" + std::to_string(g_watchFlips)
                    + " tick=" + std::to_string((long long)g_tick)
                    + " -> " + (g_realtimeOverride ? "realtime" : "FAST/pending"));
            }
        }
        // Return leg: from realtime back to frame skipping. Keep it the same 2 lines as the
        // last branch of the F8 cycle
        if (g_watchBackTicks > 0 && g_realtimeOverride && g_watchStartTick >= 0
            && g_tick - g_watchStartTick >= g_watchBackTicks) {
            g_realtimeOverride = false; watchSpeedSet(WATCH_SPEED_1X);
            g_watchBackTicks = 0;
            log::info("watchback: tick={} -> FAST (frame skip again)", (long long)g_tick);
            writeResult("watchback: tick=" + std::to_string((long long)g_tick)
                + " -> FAST (frame skip again)");
        }
        // End-zone lock tracking, for BOTH branches below. GD sets m_isLocked when the end
        // sequence takes the player (and during the level-start animation, hence x > 500).
        // The sequence is driven by the scheduler = by REAL frames, so everything that
        // reasons about it (the burn below, the fast loop's pacing, the stall guards) has
        // to read a wall clock, not ticks.
        const bool lockedNow = g_started && !g_sessionOver && m_player1
                   && m_player1->m_isLocked && m_player1->getPositionX() > 500.f;
        const auto nowLock = std::chrono::steady_clock::now();
        if (!lockedNow) g_endzoneLockStart = {};
        else if (g_endzoneLockStart.time_since_epoch().count() == 0)
            g_endzoneLockStart = nowLock;
        if (g_started && !g_sessionOver && g_cfg.fastdt > 0 && !g_realtimeOverride) {
            // Visual effects while not rendering are not deleted but "advanced to
            // completion". GD's timed effects clean up by "the action removing itself",
            // but in the fast loop actions advance only once per rendered frame and pile
            // up, so advance the actions by the game time advanced in this frame.
            // Do not advance the whole scheduler (the game layer's update hangs off it too
            // and the physics would break). Advance only the action manager (actions are
            // for visual effects only)
            if (g_fxSweep && g_cfg.skipRender) {
                float gameSecs = g_cfg.fastdt * std::max(1, g_cfg.fastloops);
                if (auto* d = cocos2d::CCDirector::sharedDirector())
                    if (auto* am = d->getActionManager())
                        static_cast<ActionManagerTick*>(am)->update(gameSecs);
                // CCCircleWave removes itself via the scheduler, not an action, so it gets
                // a separate sweep
                sweepCircleWaves();
            }
            // Fast mode: run a large number of physics ticks per rendered frame.
            // ...except while the END SEQUENCE holds the player (lockedNow): its
            // completion is scheduler-driven, i.e. it needs REAL frames, and pumping
            // 1,800 physics ticks a frame only inflates g_tick while the animation
            // stands still -- 3,000 "stillness" ticks pass in under two frames and the
            // tick-based stall guard was killing legitimate clears at the end wall
            // (lv8/11/17; see ENDZONE_KILL_AFTER_SEC). One tick per frame lets wall
            // time flow, the sequence finish and levelComplete fire.
            stallwatch::Mark fm(stallwatch::FASTLOOP);
            const int fastLoops = lockedNow ? 1 : std::max(1, g_cfg.fastloops);
            if (lockedNow) {
                static long long s_lockLogged = -1;
                if (s_lockLogged != g_attempt) {
                    s_lockLogged = g_attempt;
                    writeResult("endzone: player locked at x="
                        + std::to_string((int)m_player1->getPositionX())
                        + " tick=" + std::to_string((long long)g_tick)
                        + " - dropping to 1 tick/frame for the end sequence");
                }
            }
            for (int i = 0; i < fastLoops; ++i) {
                if (g_sessionOver) break;
                // The END SEQUENCE took the player mid-batch: stop pumping ticks
                // right here, so the next frames run at 1 tick/frame (lockedNow
                // above) and wall time reaches the scheduler. Without this the
                // whole pin-to-stillness window fits inside ONE batch.
                if (m_player1 && m_player1->m_isLocked
                    && m_player1->getPositionX() > 500.f) break;
                // Serve mode: once the one supplied attempt is over, stop inside this loop.
                // The frame boundary is too late (thousands of ticks per frame, so hundreds
                // of attempts would run on their own)
                if (g_serveMode && g_serveWait) break;
                if (g_serveReset) {
                    g_serveReset = false;
                    if (auto* pl = PlayLayer::get()) pl->resetLevel();
                }
                { stallwatch::Mark gm(stallwatch::GD_UPDATE);
                  GJBaseGameLayer::update(g_cfg.fastdt); }
            }
        } else {
            // F8 spectating speed (1x/2x/4x). Rather than increasing dt, call several times
            // with the same dt (doubling dt shifts the position of processing tied to frame
            // boundaries). Below 1x is made on the dt side (the g_watchSpeed < 1 branch
            // above), so here it truncates to 1 call. Only 1x and above become the loop
            // count
            int loops = (g_started && !g_sessionOver)
                            ? std::max(1, (int)g_watchSpeed) : 1;
            // A seek (the seek bar, the arrows) rides this same loop rather than the notch,
            // which stops at 16x. It only ever RAISES the count, so the arrows keep meaning what
            // they meant and the level is back at the user's own speed the moment it ends. The
            // count falls as the target approaches, so it decelerates into it.
            //
            // AND IT PINS dt. The note above is the whole reason: the fast side of the speed has
            // to be more CALLS at the same dt, never a bigger dt, or processing tied to frame
            // boundaries lands somewhere else. During a seek the frames are heavy by
            // construction -- hundreds of batches each -- so the real dt inflates, and passing
            // that through moved exactly the thing the note forbids moving. Measured on lv22: a
            // rewind replayed the plan into a death it does not have at 1x. 1/60 is the value
            // the solve's own verification replays use (cfg fastdt), where the plan reproduces
            // exactly, and it also makes the seek's speed independent of slow motion for free.
            const bool seekNow = g_started && !g_sessionOver && itermap::seeking();
            const float stepDt = seekNow ? (1.f / 60.f) : dt;
            if (g_started && !g_sessionOver)
                loops = std::max(loops, itermap::seekLoops(stepDt));
            // The game is STOPPED: run nothing. Normally the pause branch far above has already
            // returned and this is unreachable -- but a seek that lands puts the stop back AFTER
            // that branch has been passed, so the arrival frame would run its ordinary batch on
            // top of a game that is supposed to be frozen. Measured: a backward frame-step landed
            // exactly where it was asked to and the tick had crept 8 further on by the next
            // keypress, which made a sequence of steps drift the wrong way.
            if (g_paused || probe::g_pause) loops = 0;
            // Burning the end animation (a safety net). The end animation is a GD action =
            // driven by rendered frames, so running extra physics does not shorten real
            // time, only inflates ticks -- in realtime replay, correct behaviour is to not
            // enter the burn. Enter only when the lock persists 3 s of real time without
            // completing (the clock must be real time; measuring frame-driven effects in
            // game time is meaningless). GD also sets m_isLocked during the level-start
            // animation, so add the condition that the player has progressed (x>500), and
            // always revert once the lock releases (leave no latch)
            // lockedNow / nowLock / g_endzoneLockStart are maintained once above the
            // branch, shared with the fast loop's end-sequence pacing.
            bool locked = lockedNow && g_endzoneBurnOn
                && std::chrono::duration<double>(nowLock - g_endzoneLockStart).count()
                   >= ENDZONE_BURN_AFTER_SEC;
            if (!locked && g_endzoneBurn && (!m_player1 || !m_player1->m_isLocked)) {
                g_endzoneBurn = false;   // burn over: restore rendering
            }
            if (locked) {
                loops = std::max(loops, 1800);
                // Skip rendering during the burn too (1800 updates per frame with rendering
                // on is too heavy and looks like a freeze). Log only the moment the burn
                // starts
                if (!g_endzoneBurn) {
                    g_endzoneStart = nowLock;
                    writeResult("endzone: still locked "
                        + std::to_string((int)ENDZONE_BURN_AFTER_SEC)
                        + "s after the goal (tick=" + std::to_string((long long)g_tick)
                        + ") -> burning the end animation (render off, 1800 ticks/frame)");
                }
                g_endzoneBurn = true;
            }
            for (int i = 0; i < loops; ++i) {
                // `i > 0` is required: with an unconditional break, update is never called
                // after the session ends and the game freezes. The only thing to stop is
                // "keep running multiple times after the end"
                if (i > 0 && g_sessionOver) break;
                stallwatch::Mark gm(stallwatch::GD_UPDATE);
                GJBaseGameLayer::update(stepDt);   // pinned while seeking; see above
            }
        }
        // Post-session heartbeat. "Freezes after replay" cannot be told from a hang, so
        // leave numbers on whether updates are running / whether rendering is being
        // skipped. Do not gate on g_started (if it drops after a retry, the heartbeat
        // would stop too)
        if (g_sessionOver || g_retryDone) {
            static int s_beats = 0;
            static std::chrono::steady_clock::time_point s_last{};
            static std::chrono::steady_clock::time_point s_end = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            // Fire the equivalent of a retry (cfg `retryafter`)
            if (g_retryAfterSec > 0.0 && !g_retryDone
                && std::chrono::duration<double>(now - s_end).count() >= g_retryAfterSec) {
                g_retryDone = true;
                if (auto* pl = PlayLayer::get()) {
                    writeResult("retry: calling resetLevel (what the Retry button does)");
                    flushAll();
                    pl->resetLevel();
                }
            }
            if (s_beats < 24
                && std::chrono::duration<double>(now - s_last).count() > 2.0) {
                s_last = now;
                ++s_beats;
                char b[224];
                // So that whoever is stopping things can be named, line up GD's own stop
                // flags and the call count of the physics entry point (processCommands)
                auto* pl = PlayLayer::get();
                snprintf(b, sizeof(b),
                    "post-session heartbeat %d: tick=%lld x=%.1f started=%d over=%d "
                    "paused=%d gdStarted=%d endAnim=%d ourPause=%d probePause=%d "
                    "pcCalls=%lld visitDraws=%lld",
                    s_beats, (long long)g_tick,
                    m_player1 ? m_player1->getPositionX() : -1.f,
                    g_started ? 1 : 0, g_sessionOver ? 1 : 0,
                    pl ? (pl->m_isPaused ? 1 : 0) : -1,
                    pl ? (pl->m_started ? 1 : 0) : -1,
                    pl ? (pl->m_levelEndAnimationStarted ? 1 : 0) : -1,
                    g_paused ? 1 : 0, probe::g_pause ? 1 : 0,
                    g_pcCalls, g_visitDraws);
                writeResult(b);
                flushAll();
            }
        }
        ev("BGL_update_post", dt);
    }

    void visit() {
        // Self-recovery: if the game entered pause by some path, release it (a safety net
        // against callers other than the pauseGame hook). While paused no update arrives,
        // so this can only be noticed on the render side. Must come before the render
        // skip (after it, it would never run with skiprender=1)
        if (g_started && !g_sessionOver) {
            if (auto* pl = PlayLayer::get())
                if (pl->m_isPaused) {
                    ++g_unfocusPauseBlocked;
                    if (g_unfocusPauseBlocked <= 5)
                        writeResult("pause: found the game paused mid-session -> resuming"
                            " (tick=" + std::to_string(g_tick) + ")");
                    if (auto* pause = pl->getChildByType<PauseLayer>(0)) pause->onResume(nullptr);
                }
        }
        // In fast mode, skip rendering the game layer entirely.
        // But render in realtime mode (F8) -- so the user can spectate.
        // The HUD is a child of the scene, so it is drawn separately even when skipped here.
        if (renderSuppressed()) {   // fast mode's own skip, or F8
            ++g_visitSkips; return;
        }
        // A section search in flight owns the world: it restores a checkpoint tens of thousands
        // of times and update() returns before updateVisibility, so the visibility state the
        // draw would walk is both stale and being rewritten underneath it -- the hazard the SEH
        // guard below exists for, once per frame for the length of the search. The HUD hangs off
        // the SCENE and is drawn either way, which is what the operator needs to see here.
        if (secsolve::inFlight()) { ++g_visitSkips; return; }
        if (g_endzoneBurn) { ++g_visitSkips; return; }   // burning the end animation (see below)
        ++g_visitDraws;
        hookdepth::Guard hg(hookdepth::VISIT);
        // The first visit after a render skip walks over a stale visibility state, so it
        // goes through an SEH guard (render preparation only; unrelated to physics or
        // determinism)
        safeVisit();
    }

    void safeVisit() {
        __try {
            GJBaseGameLayer::visit();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            logVisitCrashSwallowed();
        }
    }

    // ---- Section-limited GD solver (read the design notes in src/solver/secsolve.hpp) ----
    // Breadth-first per layer. Each node is represented as "the input sequence from the
    // section head", and on every expansion the state is rebuilt by: restore to the
    // section checkpoint -> replay the prefix -> branch. Not keeping a checkpoint per layer
    // avoids the "hold a huge number of full object states" that caused the old
    // implementation's OOM. Replaying the prefix costs about 0.007ms per tick, cheap
    // compared with a restore (2.02ms).
    void secRestore(PlayLayer* pl) { secRestoreFrom(pl, g_ckpt); }

    // Rebuild a held button without an edge.
    //
    // Why pushButton (= handleButton(true)) is not called: the "action" of the press is
    // already in the restored state. Calling it would be a second firing.
    // For ship / wave the press is a sustained quantity so the second firing is invisible,
    // but for spider the press is the teleport itself, so that one call makes the
    // trajectory a different thing.
    // Measured (lv22, depth 128 from t=1664, checkpoint path): the search exit was
    // x=3106.5 while the plain replay gave x=2143.5. y and vy matched exactly at every
    // depth, and the only split was the last 1 tick of the only run of consecutive 1s in
    // the input sequence (`secsolve_split`).
    // Dash save and restore (see the note on DashState). Neither checkpoint nor psnap
    // carries it, so hold it per node and write it back after the restore.
    void secCaptureDash(secsolve::DashState& d) {
        auto* p = m_player1;
        if (!p) { d = secsolve::DashState{}; return; }
        d.on = p->m_isDashing;
        d.ring = reinterpret_cast<GameObject*>(p->m_dashRing);
        d.x = p->m_dashX;
        d.y = p->m_dashY;
        d.angle = p->m_dashAngle;
        d.startTime = p->m_dashStartTime;
        d.lastGround = p->m_lastGroundObject;
        d.maybeLastGround = p->m_maybeLastGroundObject;
        d.preLastGround = p->m_preLastGroundObject;
        d.collided = p->m_collidedObject;
        d.collideLeft = p->m_collidingWithLeft;
        d.collideRight = p->m_collidingWithRight;
        d.slope = p->m_currentSlope;
        d.slope2 = p->m_currentSlope2;
        d.potentialSlope = p->m_currentPotentialSlope;
        d.snappedTo = p->m_objectSnappedTo;
        d.lastPortal = p->m_lastActivatedPortal;
    }
    void secRestoreDash(const secsolve::DashState& d) {
        auto* p = m_player1;
        if (!p) return;
        p->m_isDashing = d.on;
        p->m_dashRing = reinterpret_cast<DashRingObject*>(d.ring);
        p->m_dashX = d.x;
        p->m_dashY = d.y;
        p->m_dashAngle = d.angle;
        p->m_dashStartTime = d.startTime;
        p->m_lastGroundObject = d.lastGround;
        p->m_maybeLastGroundObject = d.maybeLastGround;
        p->m_preLastGroundObject = d.preLastGround;
        p->m_collidedObject = d.collided;
        p->m_collidingWithLeft = d.collideLeft;
        p->m_collidingWithRight = d.collideRight;
        p->m_currentSlope = d.slope;
        p->m_currentSlope2 = d.slope2;
        p->m_currentPotentialSlope = d.potentialSlope;
        p->m_objectSnappedTo = d.snappedTo;
        p->m_lastActivatedPortal = d.lastPortal;
    }

    // m_holdingButtons alone is not enough to restore a held button.
    // m_jumpBuffered must be set too (cfg `secjumpbuf=0` turns this off; default ON).
    //
    // History (2026-08-13, the lv22 rotated-section wall at t=12,029 / x=16,165):
    // the input sequence the section solver bought died in plain replay. The checkpoint
    // restore is bit-exact (psnapDrift=0.00), yet `secsolve_verify: FAIL deadAt=401`.
    // The split was
    // `secsolve_split: depth=262/600 x 16131.000->16128.987 vy 0.000->-8.944`:
    // only the replay side performed a mini-cube jump (8.944 = 11.18x0.8).
    // The input sequence at that depth has index 250..266 all 1 = held.
    // Measured the same day: a cube that lands while still held re-jumps
    // (lv1: press once and never release -> vy=11.18 at t=61 and t=165). The restored
    // state only had m_holdingButtons set, so that re-jump did not happen.
    //
    // Same-condition A/B (same plan, same entry, same cap, checkpoint):
    //   secjumpbuf=0 -> UNVERIFIED  verify FAIL deadAt=401  split depth=262
    //   secjumpbuf=1 -> SOLVED      verify OK deadAt=-1  dy=dvy=dx=0.0000
    //                               split depth=-1 (the split disappeared)
    // The worry that a false solution slips through is covered by verify: that check only
    // passes when "search transition = plain replay". It is switchable only for A/B.
    void secArmHold() {
        auto arm = [](PlayerObject* p) {
            if (!p) return;
            p->m_holdingButtons[1] = true;
            if (secsolve::g_jumpBuf) p->m_jumpBuffered = true;
        };
        arm(m_player1);
        arm(m_player2);
        secsolve::g_held = 1;
    }

    // heldAfter: whether the button was held at the moment of the restore. The checkpoint
    // was taken in that state, so a branch that advances to the next tick while still
    // holding must pass 1. Leaving it 0 means "release and press again", i.e. searching a
    // different input sequence from the plain replay.
    void secRestoreFrom(PlayLayer* pl, CheckpointObject* cp, int heldAfter = 0) {
        if (pl->m_checkpointArray) pl->m_checkpointArray->removeAllObjects();
        pl->storeCheckpoint(cp);
        g_restorePending = true;
        pl->resetLevel();
        g_restorePending = false;
        // The button state after a restore is not necessarily "released". Assuming so and
        // setting g_held=0 means that, while actually still held, no difference shows up,
        // handleButton is not called, and the wave flies the wrong way. Release explicitly
        // first, then put it into a known state.
        g_injecting = true;
        this->handleButton(false, 1, true);
        g_injecting = false;
        secsolve::g_held = 0;
        if (heldAfter) secArmHold();
    }

    // Advance 1 tick. The input is held by secsolve::g_feed (applied on the
    // processCommands side).
    //
    // dt must be 1/240. The physics runs at 240Hz; passing the default fastdt=1/60
    // advances 4 substeps per call. Not noticing that at first, the x right after a
    // restore advancing 5.193 (= 4 x 1.298) per tick was misread as "the restore is
    // broken". The input granularity also becomes 4 ticks, changing the meaning of the
    // search.
    void secStep(int down, float) {
        secsolve::g_feed = down;
        GJBaseGameLayer::update((float)secsolve::g_dt);
    }

    // Measure whether a player-only restore can replace a full checkpoint restore
    // (cfg `secpsnap=N`).
    //
    // The measurement is exactly "is it equivalent as a branching primitive":
    //   A: full checkpoint restore -> run N ticks                       (current branch)
    //   B: level left as is, restore only the player -> run N ticks      (proposed branch)
    // B is repeated R times because that means restoring from a level state that has
    // drifted further and further ahead; if nothing in the section moves, it should match
    // A regardless of the count. In a section where it matches, one branch goes from
    // 2.02ms/0.95MB to a single memcpy.
    //
    // Never judge by survival or the terminal x (got fooled by that once today).
    // Compare y/vy every tick and report the first tick that splits.
    void runSnapCompare(PlayLayer* pl) {
        snapSweep(pl, secsolve::g_snapCmp, true);
        endSession("psnap");
    }

    static GJEffectManager* EM(GJBaseGameLayer* l) {
        return l ? l->m_effectManager : nullptr;
    }

    // Measure whether geometry moves in this section. psnap restores only the player and
    // game-layer state and does not restore GameObject positions, so in a section with
    // moving objects it disagrees with the checkpoint.
    //
    // Measured (lv20): in a non-moving section (inside wave2, target 5000) all counters
    // match the checkpoint and the solution replays plainly with dy=0.000. In a moving
    // section (ship, 231 grouped objects in the window) it splits at depth 106 -- psnap
    // misses branches the checkpoint counts as dead (dead 4 vs 2). The sweep does not
    // show it (the window is short and touches no moving object).
    // If `out` is passed, the set to restore is also built here (for `wsnap`). It is
    //   objects that actually moved (whole level) U grouped objects inside the section's
    //   x window
    // where the latter picks up "objects that move once touched". Running once plainly
    // does not move them, so the moved objects alone are not enough
    // ([[gd-touch-triggers]]).
    // Objects whose position shift makes the world a different one (mode/gravity/size/
    // dual/mirror/teleport portals). Using psnap in a section where these move makes the
    // search see "a world that does not exist" -- with a portal in a different position,
    // the mode does not change where it should (or changes where it should not).
    //
    // Measured (lv22, 2026-08-12): the ball portal (2413,59) has groups=1 and moves,
    // rising up to the player's height. The psnap search does not know that, so it
    // wiped out at the same depth with cap 400 and with cap 2000. "Detect and repair"
    // (`secverifyevery`) works against drift but not against a systematically false world.
    static bool isWorldDefiningPortal(int id) {
        switch (id) {
            case 12: case 13: case 47: case 111: case 660:
            case 745: case 1331: case 1933:            // mode
            case 10: case 11:                          // gravity
            case 99: case 101:                         // size
            case 286: case 287:                        // dual
            case 45: case 46:                          // mirror
            case 747: case 2902:                       // teleport
                return true;
            default:
                return false;
        }
    }

    int movingObjectsInSection(PlayLayer* pl, int n,
                               std::vector<GameObject*>* out = nullptr,
                               int* portalsOut = nullptr) {
        if (out) out->clear();
        auto* arr = this->m_objects;
        const int nobj = arr ? arr->count() : 0;
        if (!nobj) return 0;
        const float dt = g_cfg.fastdt;
        int held = 0;
        for (const auto& in : g_cfg.inputs)
            if (in.step <= g_ckptTick) held = in.down ? 1 : 0;
        secRestore(pl);
        for (int i = 0; i < 2; ++i) secStep(held, dt);   // the frozen ticks
        std::vector<float> px((size_t)nobj), py((size_t)nobj);
        for (int i = 0; i < nobj; ++i) {
            auto* o = static_cast<GameObject*>(arr->objectAtIndex((unsigned)i));
            px[(size_t)i] = o->getPositionX();
            py[(size_t)i] = o->getPositionY();
        }
        // The window is measured, not derived: the x range the player actually occupied,
        // plus a margin.
        //
        // Never decide from a single run. The first version measured with just one run,
        // "n ticks holding the section's input", and in the ship section it died within a
        // few dozen ticks and left the loop. Both the window and the moved objects only
        // covered the head of the section; the set of 162 was too small and it dropped the
        // d=106 deaths (4 of them) wholesale. Swallow deaths, run to the end, and take the
        // union over different input patterns.
        const bool savedNoKill = secsolve::g_noKill;
        secsolve::g_noKill = true;
        double wx0 = m_player1 ? m_player1->getPositionX() : 0.0, wx1 = wx0;
        std::vector<uint8_t> moving((size_t)nobj, 0);
        // 0=always released / 1=always held / 2=toggle every tick. What gets touched
        // changes, so put "objects moved by any one of them" into the set (the same reason
        // only the every-tick toggle broke in the sweep -- what is touched depends on the
        // trajectory).
        for (int pat = 0; pat < 3; ++pat) {
            secRestore(pl);
            for (int i = 0; i < 2; ++i) secStep(held, dt);   // the frozen ticks
            for (int i = 0; i < nobj; ++i) {
                auto* o = static_cast<GameObject*>(arr->objectAtIndex((unsigned)i));
                px[(size_t)i] = o->getPositionX();
                py[(size_t)i] = o->getPositionY();
            }
            for (int i = 0; i < n; ++i) {
                const int in = pat == 0 ? 0 : (pat == 1 ? 1 : (i & 1));
                secStep(in, dt);
                if (!m_player1) break;
                const double x = m_player1->getPositionX();
                wx0 = std::min(wx0, x); wx1 = std::max(wx1, x);
            }
            for (int i = 0; i < nobj; ++i) {
                auto* o = static_cast<GameObject*>(arr->objectAtIndex((unsigned)i));
                if (std::fabs(o->getPositionX() - px[(size_t)i]) > 1e-4
                    || std::fabs(o->getPositionY() - py[(size_t)i]) > 1e-4)
                    moving[(size_t)i] = 1;
            }
        }
        secsolve::g_noKill = savedNoKill;
        wx0 -= 900.0; wx1 += 900.0;
        int moved = 0;
        if (portalsOut) *portalsOut = 0;
        for (int i = 0; i < nobj; ++i) {
            if (moving[(size_t)i]) ++moved;
            auto* o = static_cast<GameObject*>(arr->objectAtIndex((unsigned)i));
            const bool grouped = o->m_groupCount > 0 || o->m_hasGroupParent
                                 || o->m_hasAreaParent;
            const double ox = o->getPositionX();
            const bool inWindow = ox >= wx0 && ox <= wx1;
            // Count only portals that actually moved. Including "grouped objects in the
            // window" drops back to the checkpoint because of far-away portals that are
            // never touched -- measured (lv22's early cube section): a search of a few
            // seconds with psnap took over 10 minutes with the checkpoint. Objects that
            // move once touched may be missed, but the leaf cross-check and DOOMED catch
            // that (a false solution is rejected by the verdict).
            if (portalsOut && isWorldDefiningPortal(o->m_objectID)
                && moving[(size_t)i])
                ++*portalsOut;
            if (!out) continue;
            if (moving[(size_t)i] || (grouped && inWindow))
                out->push_back(o);
        }
        return moved;
    }

    // Look at the world side directly (cfg `secworld=N`).
    // Compare the world after a checkpoint restore with the world after a psnap restore,
    // object by object.
    void runWorldDiff(PlayLayer* pl) {
        using namespace secsolve;
        const float dt = g_cfg.fastdt;
        const int N = g_worldDiff;
        char b[256];
        auto* arr = this->m_objects;
        const int nobj = arr ? arr->count() : 0;
        // A per-member fingerprint (folded over all objects). Emitting per object could
        // only say "19,671 differ" without telling what differs.
        // The first 3 are position and rotation.
        const auto& gos = psnap::goScalars();
        const auto& gbs = psnap::gbScalars();
        // First half = object side (folded over all objects), second half = game-layer
        // side. There is only one layer, so raw bytes rather than a fold.
        const size_t NOBJM = gos.size() + 3;
        const size_t NCNT_ = 21;                 // must match cnts below
        const size_t NM = NOBJM + gbs.size() + NCNT_;
        // The element counts of the OPAQUE members (containers) go into the fingerprint
        // too. Of the 261, 157 are visual-only (sprite batches etc.), but the remaining 64
        // include things that affect physics: m_sections (the spatial index for
        // collisions), m_groupDict, m_effectManager, the spawn queue. They cannot be
        // tracked by bytes, so first only check "do the counts disagree".
        struct Cnt { const char* name; std::function<long long()> get; };
        const std::vector<Cnt> cnts = {
            {"m_objects", [&] { return (long long)(this->m_objects ? this->m_objects->count() : -1); }},
            {"m_collisionBlocks", [&] { return (long long)(this->m_collisionBlocks ? this->m_collisionBlocks->count() : -1); }},
            {"m_spawnObjectsArray", [&] { return (long long)(this->m_spawnObjectsArray ? this->m_spawnObjectsArray->count() : -1); }},
            {"m_disabledObjects", [&] { return (long long)this->m_disabledObjects.size(); }},
            {"m_areaObjects", [&] { return (long long)this->m_areaObjects.size(); }},
            {"m_processedAreaObjects", [&] { return (long long)this->m_processedAreaObjects.size(); }},
            {"m_visibleObjects", [&] { return (long long)this->m_visibleObjects.size(); }},
            {"m_visibleObjects2", [&] { return (long long)this->m_visibleObjects2.size(); }},
            {"m_groups", [&] { return (long long)this->m_groups.size(); }},
            {"m_staticGroups", [&] { return (long long)this->m_staticGroups.size(); }},
            {"m_sections", [&] { return (long long)this->m_sections.size(); }},
            // ---- Contents of GJEffectManager ----
            // What CheckpointObject saves wholesale as `EffectManagerState`. This is the
            // set GD itself decided "breaks unless restored", so look here first rather
            // than blindly widening the 261 (user suggestion). It holds the trigger queue
            // and the in-progress group commands = the state while moving geometry is
            // in motion.
            {"em.pulseVec", [&] { return EM(this) ? (long long)EM(this)->m_pulseEffectVector.size() : -1; }},
            {"em.pulseMap", [&] { return EM(this) ? (long long)EM(this)->m_pulseEffectMap.size() : -1; }},
            {"em.opacityMap", [&] { return EM(this) ? (long long)EM(this)->m_opacityEffectMap.size() : -1; }},
            {"em.touchToggle", [&] { return EM(this) ? (long long)EM(this)->m_unkVector1e0.size() : -1; }},
            {"em.countTrig", [&] { return EM(this) ? (long long)EM(this)->m_countTriggerActions.size() : -1; }},
            {"em.collisionTrig", [&] { return EM(this) ? (long long)EM(this)->m_unkVector230.size() : -1; }},
            {"em.toggleTrig", [&] { return EM(this) ? (long long)EM(this)->m_unkVector248.size() : -1; }},
            {"em.itemCount", [&] { return EM(this) ? (long long)EM(this)->m_itemCountMap.size() : -1; }},
            {"em.timerItem", [&] { return EM(this) ? (long long)EM(this)->m_timerItemMap.size() : -1; }},
            {"em.timerTrig", [&] { return EM(this) ? (long long)EM(this)->m_unkMap3f8.size() : -1; }},
        };
        const size_t NCNT = cnts.size();
        auto sig = [&](std::vector<uint64_t>& out) {
            out.assign(NM, 1469598103934665603ull);
            for (size_t k = 0; k < gbs.size(); ++k) {
                uint64_t h = 1469598103934665603ull;
                const uint8_t* p = (const uint8_t*)this + gbs[k].off;
                for (size_t j = 0; j < gbs[k].size; ++j) {
                    h ^= p[j]; h *= 1099511628211ull;
                }
                out[NOBJM + k] = h;
            }
            for (size_t k = 0; k < NCNT; ++k)
                out[NOBJM + gbs.size() + k] = (uint64_t)cnts[k].get();
            for (int i = 0; i < nobj; ++i) {
                auto* o = static_cast<GameObject*>(
                    arr->objectAtIndex((unsigned)i));
                auto mix = [](uint64_t& h, const void* p, size_t n) {
                    const uint8_t* b = (const uint8_t*)p;
                    for (size_t j = 0; j < n; ++j) {
                        h ^= b[j]; h *= 1099511628211ull;
                    }
                };
                const float px = o->getPositionX(), py = o->getPositionY(),
                            pr = o->getRotation();
                mix(out[0], &px, 4); mix(out[1], &py, 4); mix(out[2], &pr, 4);
                for (size_t k = 0; k < gos.size(); ++k)
                    mix(out[k + 3], (const uint8_t*)o + gos[k].off, gos[k].size);
            }
        };
        auto nameOf = [&](size_t k) -> const char* {
            if (k == 0) return "<x>";
            if (k == 1) return "<y>";
            if (k == 2) return "<rot>";
            if (k < NOBJM) return gos[k - 3].name;
            if (k < NOBJM + gbs.size()) return gbs[k - NOBJM].name;
            return cnts[k - NOBJM - gbs.size()].name;
        };
        auto sideOf = [&](size_t k) {
            return k < NOBJM ? "obj"
                 : k < NOBJM + gbs.size() ? "LAYER" : "COUNT";
        };
        snprintf(b, sizeof(b), "world: objects=%d scalarsPerObj=%zu N=%d",
                 nobj, psnap::goScalars().size(), N);
        writeResult(b);
        if (!nobj) { endSession("world"); return; }

        int held = 0;
        for (const auto& in : g_cfg.inputs)
            if (in.step <= g_ckptTick) held = in.down ? 1 : 0;
        g_active = true;
        int freeze = 0;
        std::vector<uint64_t> base, again, after;
        std::vector<uint8_t> snapBytes;

        // (1) The world right after a checkpoint restore
        secRestore(pl);
        {
            const double xf = m_player1 ? m_player1->getPositionX() : 0.0;
            for (int i = 0; i < 8; ++i) {
                secStep(held, dt);
                if (m_player1 && m_player1->getPositionX() != xf) break;
                ++freeze;
            }
        }
        secRestore(pl);
        for (int i = 0; i < freeze; ++i) secStep(held, dt);
        sig(base);
        psnap::capture(m_player1, this, snapBytes);

        // (2) The same thing once more -> check whether the checkpoint restore itself is
        // deterministic (the denominator)
        for (int i = 0; i < N; ++i) secStep(held, dt);
        secRestore(pl);
        for (int i = 0; i < freeze; ++i) secStep(held, dt);
        sig(again);
        // Members that differ between two checkpoint restores cannot be used for the
        // comparison at all (frame-derived caches etc.). The denominator is decided
        // automatically here.
        std::vector<uint8_t> stable(NM, 1);
        size_t nStable = 0;
        for (size_t k = 0; k < NM; ++k) {
            stable[k] = (again[k] == base[k]) ? 1 : 0;
            nStable += stable[k];
        }
        snprintf(b, sizeof(b),
                 "world: CP->run%d->CP stable=%zu / %zu members "
                 "(the rest move on the restore itself, so they are not compared)",
                 N, nStable, NM);
        writeResult(b);
        for (size_t k = 0; k < NM; ++k)
            if (!stable[k]) {
                snprintf(b, sizeof(b), "world:   unstable across CP [%s] %s",
                         sideOf(k), nameOf(k));
                writeResult(b);
            }
        // If only positions fluctuate, "how many objects fail to return fully" is directly
        // the answer to "does the checkpoint restore moving geometry". Update 32's "the
        // checkpoint is bit-exact including moving geometry" only looked at the player's
        // x/y, so this is the first direct check.
        if (!stable[0] || !stable[1] || !stable[2]) {
            std::vector<float> px((size_t)nobj), py((size_t)nobj);
            for (int i = 0; i < nobj; ++i) {
                auto* o = static_cast<GameObject*>(arr->objectAtIndex((unsigned)i));
                px[(size_t)i] = o->getPositionX();
                py[(size_t)i] = o->getPositionY();
            }
            secRestore(pl);
            for (int i = 0; i < freeze; ++i) secStep(held, dt);
            int moved = 0; double worst = 0.0; int worstIdx = -1;
            for (int i = 0; i < nobj; ++i) {
                auto* o = static_cast<GameObject*>(arr->objectAtIndex((unsigned)i));
                const double d = std::fabs(o->getPositionX() - px[(size_t)i])
                               + std::fabs(o->getPositionY() - py[(size_t)i]);
                if (d > 1e-4) {
                    ++moved;
                    if (d > worst) { worst = d; worstIdx = i; }
                }
            }
            snprintf(b, sizeof(b),
                     "world: objects at a different position after two CP restores"
                     " = %d / %d (worst %.2fpx, obj[%d])",
                     moved, nobj, worst, worstIdx);
            writeResult(b);
        }

        // (3) Run N ticks, then restore only the player with psnap = what the search does
        for (int i = 0; i < N; ++i) secStep(held, dt);
        psnap::restore(m_player1, this, snapBytes.data());
        g_injecting = true;
        this->handleButton(false, 1, true);
        g_injecting = false;
        g_held = 0;
        sig(after);
        size_t nDiff = 0, shown = 0;
        for (size_t k = 0; k < NM; ++k) {
            if (!stable[k] || after[k] == base[k]) continue;
            ++nDiff;
            if (shown < 40) {
                snprintf(b, sizeof(b), "world:   psnap differs [%s] %s",
                         sideOf(k), nameOf(k));
                writeResult(b);
                ++shown;
            }
        }
        snprintf(b, sizeof(b),
                 "world: CP->run%d->psnap differs=%zu / %zu stable members",
                 N, nDiff, nStable);
        writeResult(b);
        g_active = false;
        endSession("world");
    }

    // The sweep proper. Returns whether psnap may be used as the branching primitive in
    // this section. If verbose, emit every line (the cfg `secpsnap=N` diagnostic);
    // otherwise just the verdict.
    bool snapSweep(PlayLayer* pl, int N, bool verbose) {
        using namespace secsolve;
        const float dt = g_cfg.fastdt;
        char b[256];
        if (verbose) {
            snprintf(b, sizeof(b),
                     "psnap: sizeof(PlayerObject)=%zu scalars=%zu copyBytes=%zu",
                     sizeof(PlayerObject), psnap::scalarCount(),
                     psnap::maskedBytes());
            writeResult(b);
        }
        // The held state at the section head (the value of pattern "keep")
        int held0 = 0;
        for (const auto& in : g_cfg.inputs)
            if (in.step <= g_ckptTick) held0 = in.down ? 1 : 0;

        // Input patterns. The every-tick toggle must be included. The layered BFS branches
        // both ways every tick, so psnap is unusable unless hold boundaries pass -- that is
        // exactly where the old warp historically broke (bit-identical only on non-hold
        // ticks).
        struct Pat { const char* name; int period; int base; };
        // period 0 = constant (keep emitting base) / period k = toggle every k ticks
        const Pat PATS[] = {
            {"keep",    0, held0}, {"hold", 0, 1}, {"release", 0, 0},
            {"alt1",    1, 0},     {"alt2", 2, 0}, {"alt4",    4, 0},
            {"alt8",    8, 0},     {"alt16", 16, 0},
        };
        auto inputAt = [](const Pat& p, int i) -> int {
            if (p.period <= 0) return p.base;
            return ((i / p.period) & 1) ^ p.base;
        };
        // How many ticks to run the world ahead before restoring. The real search expands
        // 100,000 times per section, so branching with psnap alone lets the world get that
        // far ahead. "How far it may drift" decides the period K at which a checkpoint is
        // re-inserted.
        const int DRIFTS_FULL[] = {0, 1, 2, 4, 8, 16, 32, 64, 128, 512, 2000, 10000};
        const int DRIFTS_QUICK[] = {0, 1, 8, 64, 512, 5000};
        const int* DRIFTS = verbose ? DRIFTS_FULL : DRIFTS_QUICK;
        const size_t NDRIFT = verbose ? sizeof(DRIFTS_FULL) / sizeof(int)
                                      : sizeof(DRIFTS_QUICK) / sizeof(int);
        bool allOk = true;

        const bool wasActive = g_active;
        g_active = true;
        std::vector<double> refY, refV;
        std::vector<uint8_t> refFlags;   // mode|grounded<<4|mini<<5|flip<<6
        std::vector<uint8_t> snapBytes;
        double sx = 0.0, sy = 0.0;
        int freeze = 0;
        auto flagsOf = [&](PlayerObject* p) -> uint8_t {
            return (uint8_t)((int)modeIdx(p)
                             | ((p->m_isOnGround ? 1 : 0) << 4)
                             | ((p->m_vehicleSize < 0.9f ? 1 : 0) << 5)
                             | ((p->m_isUpsideDown ? 1 : 0) << 6));
        };
        const Pat* pat = &PATS[0];

        // ---- A: reference run from a full checkpoint restore. Take the snapshot right
        // after the checkpoint ----
        //
        // Take the reference over a length that survives. Running it until death puts the
        // level into the "attempt over" state, after which restoring the player's bytes
        // advances no physics at all (the first version stepped on this, and dy became the
        // reference's fall distance itself = 152px. It looks like the player side is
        // broken, but it had simply stopped). If it dies, cut it shorter.
        auto refPass = [&](int n) -> int {
            secRestore(pl);
            if (freeze == 0) {
                const double xf = m_player1 ? m_player1->getPositionX() : 0.0;
                for (int i = 0; i < 8; ++i) {
                    secStep(inputAt(*pat, 0), dt);
                    if (m_player1 && m_player1->getPositionX() != xf) break;
                    ++freeze;
                }
                secRestore(pl);
            }
            for (int i = 0; i < freeze; ++i) secStep(inputAt(*pat, 0), dt);
            if (!m_player1) return -2;
            psnap::capture(m_player1, this, snapBytes);
            sx = m_player1->getPositionX();
            sy = m_player1->getPositionY();
            refY.assign((size_t)n, 0.0);
            refV.assign((size_t)n, 0.0);
            refFlags.assign((size_t)n, 0);
            for (int i = 0; i < n; ++i) {
                secStep(inputAt(*pat, i), dt);
                if (!m_player1) return -2;
                refY[(size_t)i] = m_player1->getPositionY();
                refV[(size_t)i] = (double)m_player1->m_yVelocity;
                refFlags[(size_t)i] = flagsOf(m_player1);
                if (m_player1->m_isDead) return i;
            }
            return -1;
        };

        // After a restore, rebuild the button state with the same steps as the checkpoint
        // path. `m_holdingButtons` is on the PRESERVE side (not injected), so skipping this
        // means comparing "a ship that is pressing" with "a ship that is not" (the first
        // version split by -109px because of that).
        auto rearmButton = [&]() {
            g_injecting = true;
            this->handleButton(false, 1, true);
            g_injecting = false;
            g_held = 0;
        };

        for (const Pat& P : PATS) {
            pat = &P;
            int n = N;
            int refDied = refPass(n);
            if (refDied >= 0) {
                n = refDied - 2;
                if (n < 4) {
                    snprintf(b, sizeof(b),
                             "psnap: pat=%s SKIP - the reference dies after "
                             "%d tick", P.name, refDied);
                    writeResult(b);
                    continue;
                }
                refDied = refPass(n);
            }
            if (refDied != -1) {
                snprintf(b, sizeof(b), "psnap: pat=%s SKIP - unstable reference",
                         P.name);
                writeResult(b);
                continue;
            }

            for (size_t di = 0; di < NDRIFT; ++di) {
                const int D = DRIFTS[di];
                // Every time, return the world to the checkpoint first, then advance exactly
                // D ticks. Otherwise "drift=0" is actually a state shifted by the reference
                // run (n ticks), and tolerance to small drifts cannot be measured (the first
                // version was like that).
                secRestore(pl);
                for (int i = 0; i < freeze; ++i) secStep(inputAt(P, 0), dt);
                // Advance only the world by D ticks. The player is restored and pinned
                // every tick so it does not die; only the world's phase drifts (= what
                // happens when branching with psnap continuously without inserting a
                // checkpoint).
                for (int d = 0; d < D; ++d) {
                    psnap::restore(m_player1, this, snapBytes.data());
                    rearmButton();
                    secStep(inputAt(P, 0), dt);
                    if (!m_player1 || m_player1->m_isDead) break;
                }
                const auto t0 = std::chrono::steady_clock::now();
                psnap::restore(m_player1, this, snapBytes.data());
                const double us = std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - t0).count();
                rearmButton();
                int firstBad = -1, died = -1, flagBad = -1;
                double maxDy = 0.0, maxDv = 0.0;
                for (int i = 0; i < n; ++i) {
                    secStep(inputAt(P, i), dt);
                    if (!m_player1) break;
                    if (m_player1->m_isDead) { died = i; break; }
                    const double dy = m_player1->getPositionY() - refY[(size_t)i];
                    const double dv = (double)m_player1->m_yVelocity - refV[(size_t)i];
                    if (std::fabs(dy) > std::fabs(maxDy)) maxDy = dy;
                    if (std::fabs(dv) > std::fabs(maxDv)) maxDv = dv;
                    if (firstBad < 0 && (std::fabs(dy) > 1e-4 || std::fabs(dv) > 1e-4))
                        firstBad = i;
                    if (flagBad < 0 && flagsOf(m_player1) != refFlags[(size_t)i])
                        flagBad = i;
                }
                const bool bad = (firstBad >= 0 || flagBad >= 0 || died >= 0);
                if (verbose || bad) {
                    snprintf(b, sizeof(b),
                             "psnap: pat=%-7s drift=%-5d n=%-4d restoreUs=%5.1f "
                             "firstDiff=%-4d flagDiff=%-4d maxDy=%9.4f "
                             "maxDvy=%8.4f died=%d",
                             P.name, D, n, us, firstBad, flagBad, maxDy, maxDv,
                             died);
                    writeResult(b);
                }
                if (bad) {
                    allOk = false;
                    break;   // this pattern split; larger drifts are not examined
                }
            }
            // ---- C: chain test. Measures the same usage as the search ----
            //
            // The sweep above only looks at "take one at the section head and restore it
            // repeatedly". The search captures at every node and restores at every node,
            // so if capture/restore drops even a drop of state, it accumulates with depth.
            // Indeed, even in a section where the whole sweep passed, the exit state the
            // search produced was 21px off from the plain replay. Here a capture->restore
            // is inserted every tick and compared with the plain run.
            for (int withRearm = 0; withRearm < 2; ++withRearm) {
                secRestore(pl);
                for (int i = 0; i < freeze; ++i) secStep(inputAt(P, 0), dt);
                std::vector<uint8_t> s;
                int firstBad = -1, died = -1;
                double maxDy = 0.0, maxDv = 0.0;
                for (int i = 0; i < n; ++i) {
                    if (!m_player1) break;
                    psnap::capture(m_player1, this, s);
                    psnap::restore(m_player1, this, s.data());
                    if (withRearm) rearmButton();
                    secStep(inputAt(P, i), dt);
                    if (!m_player1) break;
                    if (m_player1->m_isDead) { died = i; break; }
                    const double dy = m_player1->getPositionY() - refY[(size_t)i];
                    const double dv = (double)m_player1->m_yVelocity - refV[(size_t)i];
                    if (std::fabs(dy) > std::fabs(maxDy)) maxDy = dy;
                    if (std::fabs(dv) > std::fabs(maxDv)) maxDv = dv;
                    if (firstBad < 0 && (std::fabs(dy) > 1e-4 || std::fabs(dv) > 1e-4))
                        firstBad = i;
                }
                const bool bad = (firstBad >= 0 || died >= 0);
                if (verbose || bad) {
                    snprintf(b, sizeof(b),
                             "psnap: pat=%-7s CHAIN rearm=%d n=%-4d "
                             "firstDiff=%-4d maxDy=%9.4f maxDvy=%8.4f died=%d",
                             P.name, withRearm, n, firstBad, maxDy, maxDv, died);
                    writeResult(b);
                }
                if (bad) allOk = false;
            }
            // ---- D: rewind test. An operation only the search performs ----
            //
            // In both the sweep (restore the same one) and the chain (capture and restore
            // every tick), the player only moves forward. The search returns to a parent =
            // moves x backward. GD switches the active object sections by the player's x,
            // so rewinding x without resetting the level can leave the active state
            // inconsistent. If it splits here, then that is what CheckpointObject spends
            // 0.95MB holding.
            for (int k : {1, 2, 4, 8, 16, 32, 64, 128}) {
                if (k >= n) break;
                secRestore(pl);
                for (int i = 0; i < freeze; ++i) secStep(inputAt(P, 0), dt);
                std::vector<uint8_t> back;
                bool ok = true;
                for (int i = 0; i < n; ++i) {
                    if (i == n - k && m_player1) psnap::capture(m_player1, this, back);
                    secStep(inputAt(P, i), dt);
                    if (!m_player1 || m_player1->m_isDead) { ok = false; break; }
                }
                if (!ok || back.empty()) continue;
                // Here the world is at tick n and so is the player. Rewind x by k ticks.
                psnap::restore(m_player1, this, back.data());
                rearmButton();
                for (int j = n - k; j < n; ++j) secStep(inputAt(P, j), dt);
                if (!m_player1) break;
                const double dy = m_player1->getPositionY() - refY[(size_t)n - 1];
                const double dv = (double)m_player1->m_yVelocity - refV[(size_t)n - 1];
                const bool bad = (std::fabs(dy) > 1e-4 || std::fabs(dv) > 1e-4
                                  || m_player1->m_isDead);
                if (verbose || bad) {
                    snprintf(b, sizeof(b),
                             "psnap: pat=%-7s REWIND k=%-4d n=%-4d dy=%9.4f "
                             "dvy=%8.4f dead=%d",
                             P.name, k, n, dy, dv, m_player1->m_isDead ? 1 : 0);
                    writeResult(b);
                }
                if (bad) { allOk = false; break; }
            }
            // ---- E: wake test. The only case where the world ends up "behind" the
            // player ----
            //
            // When a branch dies the level stops, so it is woken again with a checkpoint
            // (needWake). That checkpoint restore returns the world to the section head,
            // yet the player placed next is the state at depth d. So only the world lags by
            // d ticks. The drift sweep only looks at the world being ahead, so this
            // direction had never been measured.
            for (int k : {1, 4, 16, 64}) {
                if (k >= n) break;
                secRestore(pl);
                for (int i = 0; i < freeze; ++i) secStep(inputAt(P, 0), dt);
                std::vector<uint8_t> deep;
                bool ok = true;
                for (int i = 0; i < n - k; ++i) {
                    secStep(inputAt(P, i), dt);
                    if (!m_player1 || m_player1->m_isDead) { ok = false; break; }
                }
                if (!ok) continue;
                psnap::capture(m_player1, this, deep);
                // This reproduces needWake: return only the world to the section head
                secRestore(pl);
                for (int i = 0; i < freeze; ++i) secStep(inputAt(P, 0), dt);
                psnap::restore(m_player1, this, deep.data());
                rearmButton();
                for (int j = n - k; j < n; ++j) secStep(inputAt(P, j), dt);
                if (!m_player1) break;
                const double dy = m_player1->getPositionY() - refY[(size_t)n - 1];
                const double dv = (double)m_player1->m_yVelocity - refV[(size_t)n - 1];
                const bool bad = (std::fabs(dy) > 1e-4 || std::fabs(dv) > 1e-4
                                  || m_player1->m_isDead);
                if (verbose || bad) {
                    snprintf(b, sizeof(b),
                             "psnap: pat=%-7s WAKE   k=%-4d n=%-4d dy=%9.4f "
                             "dvy=%8.4f dead=%d",
                             P.name, k, n, dy, dv, m_player1->m_isDead ? 1 : 0);
                    writeResult(b);
                }
                if (bad) { allOk = false; break; }
            }
            if (!allOk && !verbose) break;   // verdict only: settled at the first split
        }
        g_active = wasActive;
        return allOk;
    }

    // Restore fidelity check (cfg `secverify=1`). Restore to the section checkpoint, then
    // re-feed the plan's inputs from secstart on as they are, and see whether it reaches
    // the same point as the original run. The whole correctness of the section solver
    // rests on this, so pass it before searching.
    void runSectionVerify(PlayLayer* pl) {
        using namespace secsolve;
        const float dt = g_cfg.fastdt;
        // Expand the inputs from secstart on into "tick -> pressed" form.
        // The origin is neither secstart nor ckptTick: the checkpoint is made at a frame
        // boundary so it is not necessarily the requested tick (measured: requested 4700,
        // got 4701), and the restore lands on the state of ckptTick+1, so the first step
        // is fed the input of tick ckptTick+2. Off by 1 tick, re-feeding the original
        // inputs still dies.
        const long long base = g_ckptTick - 3;   // margin on both sides so secoff can act
        // seq[j] = pressed state of the plain run at tick base+j
        std::vector<uint8_t> seq((size_t)g_horizon + 12, 0);
        int held = 0;
        size_t idx = 0;
        for (long long t = 0; t <= base + (long long)seq.size(); ++t) {
            while (idx < g_cfg.inputs.size() && g_cfg.inputs[idx].step <= t) {
                held = g_cfg.inputs[idx].down ? 1 : 0;
                ++idx;
            }
            if (t >= base && (size_t)(t - base) < seq.size())
                seq[(size_t)(t - base)] = (uint8_t)held;
        }
        g_active = true;
        // Measure the frozen steps. Count them the same way the search does: the frozen
        // ticks idle with "the input at the time of the restore", and the k-th advance
        // after that produces tick base+k. Doing this with a flat array index lets the
        // frozen ticks eat two inputs and shifts the whole sequence forward (which made
        // it look like "the restore is broken").
        // seq index: tick t of the plain run is seq[t - base]. The restore lands on tick
        // ckptTick+1, so the k-th advance produces tick ckptTick+1+k. The input fed to it
        // is seq[(ckptTick+1+k) - base + g_off].
        const size_t at0 = (size_t)(g_ckptTick + 1 - base);   // = 4
        secRestore(pl);
        int freeze = 0;
        {
            const double xf = m_player1 ? m_player1->getPositionX() : 0.0;
            for (int i = 0; i < 8; ++i) {
                secStep(seq[at0], dt);
                if (m_player1 && m_player1->getPositionX() != xf) break;
                ++freeze;
            }
        }
        secRestore(pl);
        for (int i = 0; i < freeze; ++i) secStep(seq[at0], dt);
        const double x0 = m_player1 ? m_player1->getPositionX() : -1;
        const double y0 = m_player1 ? m_player1->getPositionY() : -1;
        int diedAt = -1;
        for (int i = 0; i < g_horizon; ++i) {
            const size_t si = (size_t)std::max(
                0, std::min((int)seq.size() - 1, (int)at0 + 1 + i + g_off));
            secStep(seq[si], dt);
            // With seclog=1, every step. A restore-fidelity break can only be located by
            // "from which tick it started to diverge" (the terminal difference alone cannot
            // separate a restore cause from a mid-way event).
            if ((g_log || i < 12) && m_player1) {
                char sb[192];
                snprintf(sb, sizeof(sb),
                         "secverify_step: i=%d in=%d x=%.4f y=%.4f vy=%.4f "
                         "mode=%s dead=%d",
                         i, (int)seq[si], m_player1->getPositionX(),
                         m_player1->getPositionY(),
                         (double)m_player1->m_yVelocity, modeStr(m_player1),
                         m_player1->m_isDead ? 1 : 0);
                writeResult(sb);
            }
            if (m_player1 && m_player1->m_isDead) { diedAt = i; break; }
        }
        const double x1 = m_player1 ? m_player1->getPositionX() : -1;
        const double y1 = m_player1 ? m_player1->getPositionY() : -1;
        g_active = false;
        char b[256];
        snprintf(b, sizeof(b),
                 "secverify: restoreX=%.3f restoreY=%.3f -> x=%.3f y=%.3f "
                 "diedAt=%d horizon=%d freeze=%d endTick=%lld",
                 x0, y0, x1, y1, diedAt, g_horizon, freeze,
                 (long long)(g_ckptTick + 1 + (diedAt < 0 ? g_horizon : diedAt)));
        writeResult(b);
        endSession("secverify");
    }

    // A COROUTINE (secsolve::SecTask): it suspends at a layer boundary once its slice budget is
    // spent and is resumed on the next frame, so the window, the HUD and the hotkeys stay alive
    // through a search that runs for twenty minutes. The body below is the search as it was --
    // suspending keeps its locals and lambdas in place, and no game time passes while it is
    // suspended (the driver in update() returns where the DP solve's freeze returns).
    secsolve::SecTask runSectionSolve(PlayLayer* pl) {
        using namespace secsolve;
        if (g_memAt >= 0) {
            char mb[220];
            for (auto& m : psnap::scalars()) {
                if ((long long)(m.off + m.size) <= g_memAt - 48) continue;
                if ((long long)m.off >= g_memAt + 48) continue;
                snprintf(mb, sizeof(mb), "secmem: off=%zu size=%zu %s",
                         m.off, m.size, m.name);
                writeResult(mb);
            }
        }
        const float dt = g_cfg.fastdt;
        const auto t0 = std::chrono::steady_clock::now();
        // The search runs outside tick space. Tens of thousands of ticks advance inside,
        // so the session bookkeeping (g_tick / input index) is saved and restored
        const long long savedTick = g_tick;
        const size_t savedInput = g_nextInput;

        // Choice of branching primitive (cfg `secsnap`). psnap restores only the player's
        // 3,144 bytes, so 1.4us / 3KB; the checkpoint is 2,020us / 0.95MB. But psnap does
        // not reset the level, so the world's phase keeps advancing, and in a section
        // with phase-dependent trajectories a 1 tick shift splits it (measured in lv20's
        // ship section). 2 = sweep at the section head and decide automatically. The
        // sweep checks "is it truly equivalent in this section" over 8 input patterns x
        // drift (snapSweep).
        g_snapOn = false;
        g_worldOn = false;
        g_movSet.clear();
        if (g_snapMode == 1) {
            g_snapOn = (psnap::scalarCount() > 0);
        } else if (g_snapMode == 3) {
            // Forced. Skip the sweep and run with wsnap (for diagnosis)
            g_snapOn = (psnap::scalarCount() > 0);
            if (g_snapOn) {
                movingObjectsInSection(pl, std::min(400, g_horizon), &g_movSet);
                g_worldOn = !g_movSet.empty();
            }
        } else if (g_snapMode == 4) {
            // Diagnosis only: put every object into the set. If this matches the
            // checkpoint, the cause of the split is "how the set is chosen"; if not, "what
            // is restored per object". One node becomes several MB, so use a small seccap.
            g_snapOn = (psnap::scalarCount() > 0);
            if (g_snapOn && this->m_objects) {
                for (unsigned i = 0; i < this->m_objects->count(); ++i)
                    g_movSet.push_back(static_cast<GameObject*>(
                        this->m_objects->objectAtIndex(i)));
                g_worldOn = true;
            }
        } else if (g_snapMode == 2) {
            const auto q0 = std::chrono::steady_clock::now();
            // The sweep window must cover the section length. When it was fixed at 120, in
            // a section with horizon=80 it dropped the known break at tick=105 outside the
            // window and reported PASS (trusting that, the section was solved with psnap
            // and the exit was 21px off).
            // If there is cross-check repair, no prior gate is needed (user suggestion).
            // Both the sweep and "count the moving objects" were prior "may it be used"
            // checks, kept because removing them produced false solutions. Now that the
            // frontier is matched against the ground truth every V layers, a lie is
            // rejected where it appears. The sweep takes 1.5 s and fails ship sections, so
            // skip it when V is set.
            const bool gated = g_verifyEvery <= 0;
            g_snapOn = gated ? snapSweep(pl, std::min(400, g_horizon), false)
                             : (psnap::scalarCount() > 0);
            // Even if the sweep passes, psnap must not be used plainly in a section where
            // geometry moves. The sweep's window is short, touches no moving object, and
            // passes ship sections. When there are moving objects, restore just those
            // objects as well (`wsnap`). If the set is too large one node gets heavier
            // than a checkpoint, so only then fall back to the checkpoint.
            //
            // wsnap is not used here. "Restoring the moving objects too fixes it" was
            // refuted by measurement (2026-08-07): at d=106 of lv20 ship, widening the
            // set 162->217, even restoring all 19,684 objects, the 4 branches the
            // checkpoint kills still pass through and the layer fingerprint did not move a
            // single bit. The killer is the stationary spike uid=4451, not a moving object
            // at all. Until the cause is known, moving sections stay on the checkpoint
            // (opt-in via `secsnap=3`).
            //
            // However, if `secverifyevery` is set, psnap is used even in moving sections
            // (user suggestion 2026-08-07). Even without nailing the fidelity, matching the
            // frontier every V layers by "replaying the input sequence from the section
            // checkpoint" rejects false branches there. psnap's errors only go one way
            // (missing deaths), so matching against the ground truth suffices. Replaying
            // the one solution at the end to confirm is done always.
            int moved = -1;
            int movPortals = 0;
            if (g_snapOn && gated) {
                moved = movingObjectsInSection(pl, std::min(400, g_horizon),
                                               &g_movSet, &movPortals);
                g_movSet.clear();
                if (moved > 0) g_snapOn = false;
            } else if (g_snapOn) {
                // Even with cross-check repair, a section with moving portals falls back to
                // the checkpoint. "Detect and repair" works against drift, but a world with
                // portals in different positions is not drift but a different world, and
                // the very path the search finds becomes a lie (see the note on
                // isWorldDefiningPortal).
                movingObjectsInSection(pl, std::min(400, g_horizon),
                                       nullptr, &movPortals);
                if (movPortals > 0) {
                    g_snapOn = false;
                    // When falling back to the checkpoint, lower the cap too. Cost grows
                    // super-linearly with cap and memory grows as well (measured 15GB at
                    // cap 200). The caller passes a cap meant for psnap, so clamp it here.
                    if (g_cap > 120) {
                        char cb[120];
                        snprintf(cb, sizeof(cb),
                                 "secsolve: fell back to CP, so cap %zu -> 120", g_cap);
                        writeResult(cb);
                        g_cap = 120;
                    }
                }
            }
            const double qms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - q0).count();
            char qb[256];
            snprintf(qb, sizeof(qb),
                     "secsolve: snap qualify %s (%.0f ms, movingObjects=%d, "
                     "movingPortals=%d, verifyEvery=%d) - branching with %s",
                     g_snapOn ? "PASS" : "FAIL", qms, moved, movPortals,
                     g_verifyEvery,
                     !g_snapOn ? "CheckpointObject"
                               : (moved > 0 ? "psnap + a checked correction"
                                            : "psnap (3KB/1.4us)"));
            writeResult(qb);
        }
        // Out-of-bounds ceiling (see the note on g_maxY). Look only at collidable objects,
        // and only within the x range this section reaches.
        //
        // Taking the whole level and every type is meaningless: lv22 has triggers stacked
        // at y=3,285 (decorations at 3,255), and with that as the ceiling not one arc
        // (y=3,200-3,900) can be dropped. With collidable objects only, the top around
        // here is about 900.
        g_maxYAuto = 0.0;
        if (g_maxY <= 0.0 && this->m_objects && m_player1) {
            const double x0 = m_player1->getPositionX();
            const double x1 = x0 + (double)g_horizon * 5.5 + 300.0;
            double top = 0.0;
            for (unsigned i = 0; i < this->m_objects->count(); ++i) {
                auto* o = static_cast<GameObject*>(
                    this->m_objects->objectAtIndex(i));
                const int ty = (int)o->m_objectType;
                if (ty == 7 || ty == 20) continue;      // decoration / trigger
                const double ox = o->getPositionX();
                if (ox < x0 - 300.0 || ox > x1) continue;
                top = std::max(top, (double)o->getPositionY());
            }
            if (top > 0.0) g_maxYAuto = top + 600.0;
        }
        const double killAboveY = g_maxY > 0.0 ? g_maxY : g_maxYAuto;

        g_active = true;
        // Only the search proper swallows deaths. The sweep (snapSweep) must detect death
        // to shorten its window, so do not set this before here.
        g_noKill = g_killOverride < 0 ? g_snapOn : (g_killOverride == 1);
        g_died = false;
        g_nodes.clear();
        releaseAnchors();
        g_nodes.push_back({-1, 0, 0, 0.f, 0.f, 0.f, 0});
        g_dash.assign(1, secsolve::DashState{});  // section head (input unused)

        // Frontier nodes hold their own checkpoint. In the version that replayed the
        // prefix every time, step count grew with the square of depth: 767,972 steps /
        // 154 s in one lv20 section, far above restores (20 s). With checkpoints, 1
        // expansion = 1 restore + 1 step. The key is holding only the frontier's worth;
        // holding hundreds to thousands goes back to the "hold a huge number of full
        // object states" that caused the old implementation's OOM.
        // Right after a restore there are substeps in which physics does not advance. The
        // phenomenon is in the old snapsolve notes; the count can vary with the reset path,
        // so it is measured every time. Without the correction, the 1 step of "1 expansion
        // = restore + 1 step" is entirely a frozen step, and the frontier spins while x
        // never advances (which actually happened).
        int freeze = 0;
        {
            secRestoreFrom(pl, g_ckpt, g_headHeld);
            const double x0 = m_player1 ? m_player1->getPositionX() : 0.0;
            const double y0 = m_player1 ? m_player1->getPositionY() : 0.0;
            for (int i = 0; i < 8; ++i) {
                secStep(g_headHeld, dt);
                // Do not look only at x. In a rotated gameplay section x is frozen and y
                // advances, so "x does not move = physics did not advance" is always true
                // and freeze=8 is falsely detected (measured at lv22's entry t=1777). Then
                // every expansion idles 8 extra ticks and the search looks at a different
                // level. If either moved, physics advanced.
                if (m_player1 && (m_player1->getPositionX() != x0
                                  || m_player1->getPositionY() != y0)) break;
                ++freeze;
            }
        }
        // Dash state at the section head (the checkpoint does not carry it, so take it
        // ourselves)
        secRestoreFrom(pl, g_ckpt, g_headHeld);
        for (int f = 0; f < freeze; ++f) secStep(g_headHeld, dt);
        secCaptureDash(g_dash[0]);
        // Baseline for the counter delta (Node::cnt). Taken once in the section-head world.
        secsolve::g_cntBase = secsolve::countSum(this);

        g_cps.assign(1, g_ckpt);          // nodes[0]'s checkpoint is the section checkpoint itself
        // Node state on the psnap path. Unlike checkpoints it is 3KB, so holding the whole
        // frontier is light.
        std::vector<std::vector<uint8_t>> snaps;
        // The effect manager's queue (what CheckpointObject saves). Carried by C++
        // assignment, not bytes. Same index as snaps.
        std::vector<gd::vector<PulseEffectAction>> pulses;
        // GJGameState (in-progress moves/rotations, portals, object physics). The main
        // suspect.
        std::vector<GJGameState> states;
        // The player-side "already touched" bookkeeping (orbs/pads/slopes)
        std::vector<psnap::Touch> touches;
        // The pose of moving objects (`wsnap`). In a section with moving objects this is
        // the core -- do for the moving objects what psnap does for the player.
        std::vector<std::vector<uint8_t>> worlds;
        // psnap does not go through resetLevel, so it has no frozen steps (1 restore + 1
        // step advances exactly 1 tick). The sweep confirms bit-identity with exactly this
        // procedure.
        const int snapFreeze = g_snapOn ? 0 : freeze;
        // The stacking check (`secoverlay`) takes the same snapshots on the checkpoint
        // path too.
        const bool keepSnaps = g_snapOn || g_overlay != 0;
        if (keepSnaps) {
            secRestoreFrom(pl, g_ckpt, g_headHeld);
            for (int i = 0; i < freeze; ++i) secStep(g_headHeld, dt);
            snaps.resize(1);
            pulses.resize(1);
            states.resize(1);
            touches.resize(1);
            psnap::capture(m_player1, this, snaps[0]);
            psnap::captureEM(this, pulses[0]);
            psnap::captureState(this, states[0]);
            psnap::captureTouch(m_player1, touches[0]);
            if (g_worldOn) {
                worlds.resize(1);
                psnap::captureWorld(g_movSet, worlds[0]);
            }
        }
        long long rephases = 0;
        bool needWake = false;   // previous branch died -> wake the level before next expansion
        // Continued psnap branching advances the world's phase by 1 tick per expansion. A
        // safety valve that periodically pulls it back with a checkpoint, to stay within
        // the range confirmed by the sweep (5,000 ticks).
        auto rephase = [&]() {
            if (!g_snapOn) return;
            secRestoreFrom(pl, g_ckpt, g_headHeld);
            for (int i = 0; i < freeze; ++i) secStep(g_headHeld, dt);
            ++rephases;
        };
        std::vector<int> cur{0}, nxt;
        std::unordered_set<long long> seen;
        // Number of live checkpoints at once. Before a layer is pruned, cur + nxt = up to
        // 3*cap are held, so this is where to look for a return of the OOM (the old
        // implementation's failure was a mix-up of exactly this).
        long long restores = 0, steps = 0, cpMade = 0, cpPeak = 0;
        int foundLeaf = -1;
        double foundX = 0.0;
        int foundDepth = 0;
        int deepest = 0;
        // Results of the leaf cross-check (filled by evalLeaf below). Used in the report.
        g_leafVerified = false;
        g_leafDeadAt = -1;
        double verifyX = 0.0, verifyY = 0.0, verifyVy = 0.0;
        int splitAt = -1;
        double splitNodeX = 0, splitRepX = 0, splitNodeY = 0, splitRepY = 0;
        double splitNodeVy = 0, splitRepVy = 0;
        int graceOk = -1, graceDeadAt = -1;
        long long doomedLeaves = 0;      // number of exits discarded as dead ends
        double maxVerifyDrift = 0.0;     // size of psnap's lie (max diff in the cross-check pass)

        auto releaseCp = [&](int ni) {
            if (ni <= 0) return;                       // the section checkpoint is never released
            if (keepSnaps) {
                // Assignment or clear() does not give memory back. Containers only set size
                // to 0 and keep the allocated buffer, so swap with an empty temporary and
                // let the temporary carry it away.
                //
                // Getting this wrong made GD grow 27MB per layer, reaching 14GB around depth
                // 400 and putting the whole machine into swap (lv20's ship section fell to
                // 9 s/layer; psnap is normally 7ms/layer). Only snaps used swap correctly;
                // the other three were assignments.
                if ((size_t)ni < snaps.size())
                    std::vector<uint8_t>().swap(snaps[(size_t)ni]);
                if ((size_t)ni < pulses.size()) {
                    gd::vector<PulseEffectAction> tmp;
                    std::swap(pulses[(size_t)ni], tmp);
                }
                if ((size_t)ni < states.size()) {
                    GJGameState tmp;
                    std::swap(states[(size_t)ni], tmp);
                }
                if ((size_t)ni < touches.size()) {
                    psnap::Touch tmp;
                    std::swap(touches[(size_t)ni], tmp);
                }
                if ((size_t)ni < worlds.size())
                    std::vector<uint8_t>().swap(worlds[(size_t)ni]);
                if (g_snapOn) return;                  // the psnap path holds no checkpoint
            }
            if ((size_t)ni >= g_cps.size()) return;
            if (g_cps[(size_t)ni]) { g_cps[(size_t)ni]->release(); g_cps[(size_t)ni] = nullptr; }
        };

        // Is the state controllable (used for exit survivability)
        auto controllable = [](PlayerObject* p) {
            if (!p) return false;
            const int md = modeIdx(p);
            // ship / UFO / wave / swing accept input even in the air
            if (md == 1 || md == 3 || md == 4 || md == 7) return true;
            return p->m_isOnGround != 0;
        };

        // ---- Leaf cross-check (plain replay + exit survivability) -------------------
        // Verify "on the spot" when the goal is reached. Formerly the cross-check ran once
        // after stopping the search, so if the first leaf that arrived was a dead end, the
        // search ended there. The right thing is to discard the dead-end exit and keep
        // searching (there may be other paths).
        // Return 0=SOLVED / 1=UNVERIFIED (not reproduced by replay) / 2=DOOMED (exit is a
        // dead end)
        auto evalLeaf = [&](int leaf) -> int {
            g_leafVerified = false;
            g_leafDeadAt = -1;
            splitAt = -1;
            graceOk = -1;
            graceDeadAt = -1;
            verifyX = verifyY = verifyVy = 0.0;
            std::vector<uint8_t> seq;
            std::vector<int> path;
            for (int i = leaf; i > 0; i = g_nodes[(size_t)i].parent) {
                seq.push_back(g_nodes[(size_t)i].in);
                path.push_back(i);
            }
            std::reverse(seq.begin(), seq.end());
            std::reverse(path.begin(), path.end());
            secRestoreFrom(pl, g_ckpt, g_headHeld);
            for (int f = 0; f < freeze; ++f) secStep(g_headHeld, dt);
            bool dead = false;
            for (size_t i = 0; i < seq.size(); ++i) {
                g_died = false;
                secStep((int)seq[i], dt); ++steps;
                if (!m_player1 || m_player1->m_isDead || g_died) {
                    dead = true; g_leafDeadAt = (int)i + 1; break;
                }
                if (splitAt < 0) {
                    const auto& nd = g_nodes[(size_t)path[i]];
                    const double rx = m_player1->getPositionX();
                    const double ry = m_player1->getPositionY();
                    const double rv = (double)m_player1->m_yVelocity;
                    if (std::fabs(rx - (double)nd.x) > 0.02
                        || std::fabs(ry - (double)nd.y) > 0.02
                        || std::fabs(rv - (double)nd.vy) > 0.02) {
                        splitAt = (int)i + 1;
                        splitNodeX = nd.x; splitRepX = rx;
                        splitNodeY = nd.y; splitRepY = ry;
                        splitNodeVy = nd.vy; splitRepVy = rv;
                    }
                }
            }
            if (dead || !m_player1) return 1;
            verifyX = m_player1->getPositionX();
            verifyY = m_player1->getPositionY();
            verifyVy = (double)m_player1->m_yVelocity;
            if (!reachedGoal(verifyX, verifyY, (int)seq.size())) return 1;
            g_leafVerified = true;
            if (g_grace <= 0) return 0;
            // Exit survivability (see the note on g_grace). Two runs are not enough -- the
            // spider can teleport even in the air if a surface is in range, so "never
            // press" and "always hold" both dying is no proof that "input can do nothing".
            static const int kGracePeriods[] = {0, -1, 2, 3, 4, 6, 8, 12, 20, 40};
            CheckpointObject* lcp = pl->markCheckpoint();
            if (!lcp) return 0;
            lcp->retain();
            graceOk = 0;
            for (size_t pi = 0;
                 pi < sizeof(kGracePeriods) / sizeof(int) && graceOk == 0; ++pi) {
                const int per = kGracePeriods[pi];
                if (pi) {
                    secRestoreFrom(pl, lcp, 0); ++restores;
                    for (int f = 0; f < freeze; ++f) secStep(0, dt);
                }
                bool gdead = false;
                for (int i = 0; i < g_grace; ++i) {
                    const int in = per == 0 ? 0 : per < 0 ? 1
                                 : ((i % per) == 0 ? 1 : 0);
                    g_died = false;
                    secStep(in, dt); ++steps;
                    if (!m_player1 || m_player1->m_isDead || g_died) {
                        gdead = true;
                        if (pi == 0) graceDeadAt = i + 1;
                        break;
                    }
                    if (controllable(m_player1)) { graceOk = 1; break; }
                }
                if (!gdead && graceOk == 0) graceOk = 1;
            }
            lcp->release();
            return graceOk == 0 ? 2 : 0;
        };

        // Layer breakdown (cfg `seclog=1`). Report separately whether it thinned by dead /
        // dedupe / cap. If the frontier thins in a layer the cap is not binding, the wall
        // is real; if the cap is binding, it is a cap or granularity matter. Reading a wall
        // as real from "depth stops growing" alone, without this split, was the weak point
        // of update 32.
        int layDead = 0, layDup = 0, layCap = 0;
        double layMaxX = 0.0, layMinY = 0.0, layMaxY = 0.0, layMinX = 0.0;
        double deadMaxX = 0.0;
        for (int depth = 1; depth <= g_horizon && foundLeaf < 0; ++depth) {
            nxt.clear();
            seen.clear();
            layDead = layDup = layCap = 0;
            layMaxX = deadMaxX = 0.0;
            layMinY = 1e9; layMaxY = -1e9; layMinX = 1e9;
            g_depth = depth;
            for (int ni : cur) {
                if (foundLeaf >= 0) break;
                // Hand the frame back BETWEEN EXPANSIONS as well, not only between layers. A
                // layer's cost is the cap's (measured 1.7 s per layer at cap 120, lv22's band),
                // and Windows calls a window that has not pumped for five seconds dead -- so
                // layer granularity alone would put the ghost window back the moment somebody
                // raised the cap. This is the same kind of boundary as the one at the end of the
                // layer: an expansion begins by restoring, so the world between two of them is
                // not a state anything reads.
                if (g_sliceMs > 0 && sliceExpired()) co_await std::suspend_always{};
                CheckpointObject* base = nullptr;
                if (!g_snapOn) {
                    base = (size_t)ni < g_cps.size() ? g_cps[(size_t)ni] : nullptr;
                    if (!base) continue;
                } else if ((size_t)ni >= snaps.size() || snaps[(size_t)ni].empty()) {
                    continue;
                }
                for (int branch = 0; branch < 2; ++branch) {
                    g_died = false;          // pick up this step's death verdict
                    if (g_snapOn) {
                        // Deaths are swallowed, so no wake is needed. Only the periodic
                        // phase pull-back remains, as a safety valve.
                        if (g_rephase > 0 && restores > 0
                            && restores % g_rephase == 0) rephase();
                        // Restore the world first. Restoring the player triggers no
                        // collision test, but restoring the world goes through setPosition,
                        // so section re-registration runs. Keep the order fixed.
                        if (g_worldOn && (size_t)ni < worlds.size())
                            psnap::restoreWorld(this, g_movSet, worlds[(size_t)ni]);
                        psnap::restore(m_player1, this, snaps[(size_t)ni].data());
                        psnap::restoreEM(this, pulses[(size_t)ni]);
                        psnap::restoreState(this, states[(size_t)ni]);
                        psnap::restoreTouch(m_player1, touches[(size_t)ni]);
                        // Buttons are not injected (`m_holdingButtons` is PRESERVE). Put
                        // them into a known state by the same steps as the checkpoint path,
                        // then feed the branch input.
                        g_injecting = true;
                        this->handleButton(false, 1, true);
                        g_injecting = false;
                        g_held = 0;
                        // A branch that keeps holding re-holds without an edge (see the
                        // note on secArmHold)
                        if (g_nodes[(size_t)ni].in) secArmHold();
                        // Dash is not carried either (the mask does not copy pointers, so
                        // `m_dashRing` is lost). See the note on DashState.
                        if ((size_t)ni < g_dash.size())
                            secRestoreDash(g_dash[(size_t)ni]);
                        // Rebuild the candidate list. On the checkpoint path this happens by
                        // itself through the 2 frozen steps, but the psnap path has no
                        // freeze.
                        if (secsolve::g_visRefresh) {
                            this->preUpdateVisibility(0.f);
                            this->updateVisibility(0.f);
                        }
                        ++restores;
                    } else {
                        secRestoreFrom(pl, base, g_nodes[(size_t)ni].in);
                        ++restores;
                        // Dash is not in the checkpoint. Restore it here
                        if ((size_t)ni < g_dash.size())
                            secRestoreDash(g_dash[(size_t)ni]);
                        // Idle the frozen ticks with "the input that node was holding"
                        // (physics does not advance so the state is unchanged, but button
                        // continuity is kept)
                        for (int f = 0; f < snapFreeze; ++f) {
                            secStep(g_nodes[(size_t)ni].in, dt); ++steps;
                        }
                        // Stacking check. Should be the identity -- the same node's state is
                        // merely restored again with psnap after the checkpoint restore. If
                        // the output moves, psnap is the side that breaks things.
                        if (g_overlay && (size_t)ni < snaps.size()
                            && !snaps[(size_t)ni].empty()) {
                            if (g_overlay & 1)
                                psnap::restore(m_player1, this,
                                               snaps[(size_t)ni].data());
                            if (g_overlay & 2) psnap::restoreEM(this, pulses[(size_t)ni]);
                            if (g_overlay & 4) psnap::restoreState(this, states[(size_t)ni]);
                            if (g_overlay & 8) psnap::restoreTouch(m_player1, touches[(size_t)ni]);
                        }
                    }
                    // Whether m_collisionLog* actually holds content (cfg `seccollog=1`).
                    // "Hits the same object 2 ticks late" has the shape of the collision
                    // continuation bookkeeping, so it is the main suspect, but if empty the
                    // suspect vanishes entirely. Measure whether it is used before writing a
                    // restore for it.
                    if (secsolve::g_colLog && m_player1 && restores % 2000 == 1) {
                        auto cnt = [](cocos2d::CCDictionary* d) {
                            return d ? (int)d->count() : -1;
                        };
                        char cb[200];
                        snprintf(cb, sizeof(cb),
                                 "seccollog: d=%d top=%d bottom=%d left=%d right=%d "
                                 "snapped=%s collided=%s",
                                 depth, cnt(m_player1->m_collisionLogTop),
                                 cnt(m_player1->m_collisionLogBottom),
                                 cnt(m_player1->m_collisionLogLeft),
                                 cnt(m_player1->m_collisionLogRight),
                                 m_player1->m_objectSnappedTo ? "yes" : "no",
                                 m_player1->m_collidedObject ? "yes" : "no");
                        writeResult(cb);
                    }
                    if (g_winShift) {
                        this->m_leftSectionIndex += g_winShift;
                        this->m_rightSectionIndex += g_winShift;
                    }
                    secStep(branch, dt); ++steps;
                    auto* p = m_player1;
                    // Death is seen via the flag set by destroyPlayer. Nothing is actually
                    // killed, so m_isDead is not set (on the checkpoint path it is set as
                    // before). Branches that escaped upward out of bounds are discarded
                    // here (see the note on g_maxY). GD carries them a bit higher before
                    // killing, but they eat cap the whole time, so treat them as dead as
                    // soon as it is known.
                    if (p && killAboveY > 0.0
                        && (double)p->getPositionY() > killAboveY) {
                        ++layDead;
                        deadMaxX = std::max(deadMaxX, (double)p->getPositionX());
                        needWake = true;
                        continue;
                    }
                    if (!p || g_died || p->m_isDead) {
                        ++layDead;
                        if (p) deadMaxX = std::max(deadMaxX, (double)p->getPositionX());
                        needWake = true;   // only meaningful on the checkpoint path
                        continue;
                    }
                    const double px = p->getPositionX();
                    const double py = p->getPositionY();
                    const double pv = (double)p->m_yVelocity;
                    // deadband: a branch passing through the band in a forbidden mode is
                    // treated as dead (see the note on secsolve::g_secBands). Placed before
                    // the goal test -- so that an arc that "crosses and survives" inside the
                    // band is not returned as a solution.
                    if (!secsolve::g_secBands.empty()) {
                        bool banned = false;
                        for (const auto& db : secsolve::g_secBands)
                            if (px >= db.x0 && px <= db.x1
                                && py >= db.y0 && py <= db.y1
                                && (db.mode < 0 || modeIdx(p) != db.mode)) {
                                banned = true;
                                break;
                            }
                        if (banned) {
                            ++layDead;
                            deadMaxX = std::max(deadMaxX, px);
                            needWake = true;
                            continue;
                        }
                    }
                    layMaxX = std::max(layMaxX, px);
                    layMinX = std::min(layMinX, px);
                    layMinY = std::min(layMinY, py);
                    layMaxY = std::max(layMaxY, py);
                    if (reachedGoal(px, py, depth)) {
                        // After the cutoff, discard without evaluating (see the note on
                        // g_maxDoomed). The search continues but the cost does not grow.
                        if (g_maxDoomed > 0 && doomedLeaves >= g_maxDoomed)
                            continue;
                        g_nodes.push_back({ni, (uint8_t)branch,
                                           (uint8_t)(p->m_isDashing ? 1 : 0),
                                           (float)py, (float)pv, (float)px,
                                           (uint16_t)secsolve::cntNow(this)});
                        g_dash.emplace_back();
                        secCaptureDash(g_dash.back());
                        g_cps.push_back(nullptr);
                        if (keepSnaps) {
                            snaps.emplace_back(); pulses.emplace_back();
                            states.emplace_back(); touches.emplace_back();
                        }
                        if (g_worldOn) worlds.emplace_back();
                        const int cand = (int)g_nodes.size() - 1;
                        const int st = evalLeaf(cand);
                        if (st != 2) {           // SOLVED or UNVERIFIED stops the search
                            foundLeaf = cand;
                            foundX = st == 0 ? verifyX : px;
                            foundDepth = depth;
                            break;
                        }
                        // Do not stop at a dead-end exit. The search continues. The
                        // cross-check returned the world to the section head, so the next
                        // expansion starts from a restore on either path (checkpoint and
                        // psnap alike).
                        ++doomedLeaves;
                        needWake = true;
                        // Keep the last dead-end exit for the report (foundLeaf stays unset)
                        foundX = verifyX;
                        continue;
                    }
                    const int cntHere = secsolve::cntNow(this);
                    const long long k = keyOf(py, pv, (int)modeIdx(p),
                                              p->m_vehicleSize < 0.9f ? 1 : 0,
                                              p->m_isUpsideDown ? 1 : 0, branch,
                                              px, p->m_isDashing ? 1 : 0,
                                              cntHere);
                    if (!seen.insert(k).second) { ++layDup; continue; }
                    // No cap discard here. Two children per parent, so the collected count
                    // is naturally bounded by 2*|cur|. Pruning happens once the layer is
                    // complete (below).
                    if (g_snapOn) {
                        g_nodes.push_back({ni, (uint8_t)branch,
                                           (uint8_t)(p->m_isDashing ? 1 : 0),
                                           (float)py, (float)pv, (float)px,
                                           (uint16_t)cntHere});
                        g_dash.emplace_back();
                        secCaptureDash(g_dash.back());
                        g_cps.push_back(nullptr);
                        snaps.emplace_back();
                        pulses.emplace_back();
                        states.emplace_back();
                        touches.emplace_back();
                        psnap::capture(p, this, snaps.back());
                        psnap::captureEM(this, pulses.back());
                        psnap::captureState(this, states.back());
                        psnap::captureTouch(p, touches.back());
                        if (g_worldOn) {
                            worlds.emplace_back();
                            psnap::captureWorld(g_movSet, worlds.back());
                        }
                        ++cpMade;
                    } else {
                        CheckpointObject* cp = pl->markCheckpoint();
                        if (!cp) continue;             // if it cannot be made, drop that branch
                        cp->retain(); ++cpMade;
                        g_nodes.push_back({ni, (uint8_t)branch,
                                           (uint8_t)(p->m_isDashing ? 1 : 0),
                                           (float)py, (float)pv, (float)px,
                                           (uint16_t)cntHere});
                        g_dash.emplace_back();
                        secCaptureDash(g_dash.back());
                        g_cps.push_back(cp);
                        // For the stacking check. Taken at the same instant as the checkpoint
                        if (g_overlay) {
                            snaps.emplace_back(); pulses.emplace_back();
                            states.emplace_back(); touches.emplace_back();
                            psnap::capture(p, this, snaps.back());
                            psnap::captureEM(this, pulses.back());
                            psnap::captureState(this, states.back());
                            psnap::captureTouch(p, touches.back());
                        }
                    }
                    nxt.push_back((int)g_nodes.size() - 1);
                    cpPeak = std::max(cpPeak, (long long)(cur.size() + nxt.size()));
                }
            }
            // Prune the layer to the cap. Never take the first cap in insertion order.
            // Children are ordered "parent order x branch order", which is roughly
            // ascending y (if parents are in y order, their children come low branch then
            // high branch). Cutting the tail drops only the top of the frontier every
            // layer. Measured at lv20 x=6342: with granularity 0.25 + cap 200 the band
            // collapsed to 6px (197.7..202.2), while coarse granularity with no cap hit
            // (cap 400) gave 54px (197.7..251.3) -- the upside-down result that finer is
            // worse. Sort by y and take at even spacing, always keeping both band edges.
            bool capByX = false;
            if (nxt.size() > g_cap) {
                // Do not crush the x families. With two exits of a rotated gameplay
                // section, families 40px apart in x line up at the same depth. Ordering by
                // y alone and taking at even spacing keeps the y-band edges but can drop a
                // whole family. In a section where x decides the phase (= passing the
                // moving spikes), that is fatal.
                // Cut x into buckets and take from the buckets round-robin.
                // Buckets are cut by x and the "kind of state". While dashing, y is fixed,
                // so with y-ordered even spacing those nodes get buried inside and dropped.
                // The rarer the kind, the less it may be dropped (it is the path).
                std::unordered_map<long long, std::vector<int>> buckets;
                for (int ni : nxt) {
                    const auto& nd = g_nodes[(size_t)ni];
                    // cnt is a family too (same reason as dash): branches that hit are rare
                    // and get buried among pass-through branches under y-even spacing. They
                    // are the path.
                    const long long b = (long long)std::llround(nd.x / 16.0)
                                        + (nd.dash ? 1000003LL : 0LL)
                                        + (long long)nd.cnt * 2000003LL;
                    buckets[b].push_back(ni);
                }
                if (buckets.size() > 1) {
                    for (auto& kv : buckets) {
                        std::sort(kv.second.begin(), kv.second.end(),
                                  [&](int a, int b2) {
                                      const auto& na = g_nodes[(size_t)a];
                                      const auto& nb = g_nodes[(size_t)b2];
                                      if (na.y != nb.y) return na.y < nb.y;
                                      return na.vy < nb.vy;
                                  });
                    }
                    std::vector<long long> keys;
                    keys.reserve(buckets.size());
                    for (auto& kv : buckets) keys.push_back(kv.first);
                    std::sort(keys.begin(), keys.end());
                    std::vector<int> keepv;
                    keepv.reserve(g_cap);
                    std::unordered_set<int> kept;
                    // The share per bucket. Even a small family always gets one.
                    const size_t per = std::max<size_t>(1, g_cap / keys.size());
                    for (long long k : keys) {
                        auto& v = buckets[k];
                        const size_t take = std::min(per, v.size());
                        for (size_t i = 0; i < take && keepv.size() < g_cap; ++i) {
                            const size_t idx = take > 1
                                ? (size_t)((double)i * (double)(v.size() - 1)
                                           / (double)(take - 1) + 0.5)
                                : v.size() / 2;
                            if (kept.insert(v[idx]).second) keepv.push_back(v[idx]);
                        }
                    }
                    // Fill the remainder in y order (raises the band's resolution)
                    if (keepv.size() < g_cap) {
                        std::vector<int> rest;
                        for (int ni : nxt) if (!kept.count(ni)) rest.push_back(ni);
                        std::sort(rest.begin(), rest.end(), [&](int a, int b2) {
                            return g_nodes[(size_t)a].y < g_nodes[(size_t)b2].y;
                        });
                        const size_t need = g_cap - keepv.size();
                        for (size_t i = 0; i < need && i < rest.size(); ++i) {
                            const size_t idx = need > 1
                                ? (size_t)((double)i * (double)(rest.size() - 1)
                                           / (double)(need - 1) + 0.5)
                                : rest.size() / 2;
                            if (kept.insert(rest[idx]).second)
                                keepv.push_back(rest[idx]);
                        }
                    }
                    for (int ni : nxt)
                        if (!kept.count(ni)) { releaseCp(ni); ++layCap; }
                    nxt.swap(keepv);
                    capByX = true;
                }
            }
            if (nxt.size() > g_cap) {
                // Which axis the "band" forms on depends on the section. A normal section
                // spreads in y, but in a rotated gameplay section y is the clock and nearly
                // equal across branches; the spread is in x. Ordering by y and taking at
                // even spacing then means taking by "the ordering within nearly-equal y",
                // defeating the purpose of keeping the band edges. Order along the axis
                // that spreads.
                double ylo = 1e18, yhi = -1e18, xlo = 1e18, xhi = -1e18;
                for (int ni : nxt) {
                    const auto& n = g_nodes[(size_t)ni];
                    ylo = std::min(ylo, (double)n.y); yhi = std::max(yhi, (double)n.y);
                    xlo = std::min(xlo, (double)n.x); xhi = std::max(xhi, (double)n.x);
                }
                capByX = (xhi - xlo) > (yhi - ylo);
                std::sort(nxt.begin(), nxt.end(), [&](int a, int b) {
                    const auto& na = g_nodes[(size_t)a];
                    const auto& nb = g_nodes[(size_t)b];
                    if (capByX) {
                        if (na.x != nb.x) return na.x < nb.x;
                        if (na.y != nb.y) return na.y < nb.y;
                        return na.vy < nb.vy;
                    }
                    if (na.y != nb.y) return na.y < nb.y;
                    return na.vy < nb.vy;
                });
                const size_t n = nxt.size();
                std::vector<int> keepv;
                keepv.reserve(g_cap);
                std::unordered_set<int> kept;
                for (size_t i = 0; i < g_cap; ++i) {
                    const size_t idx = g_cap > 1
                        ? (size_t)((double)i * (double)(n - 1) / (double)(g_cap - 1) + 0.5)
                        : n / 2;
                    if (kept.insert(nxt[idx]).second) keepv.push_back(nxt[idx]);
                }
                for (int ni : nxt)
                    if (!kept.count(ni)) { releaseCp(ni); ++layCap; }
                nxt.swap(keepv);
            }
            // ---- Detect and repair (cfg `secverifyevery`) --------------------
            // Match the frontier nodes by replaying their input sequence from the section
            // checkpoint. psnap's errors only go in the direction of missing deaths, so
            // dropping them here leaves a frontier that is reachable on the real game only.
            // Surviving nodes have their snapshots retaken from the real state after the
            // replay (if they had drifted, that fixes them there).
            if (g_snapOn && g_verifyEvery > 0 && !nxt.empty()
                && depth % g_verifyEvery == 0) {
                std::vector<int> ok;
                ok.reserve(nxt.size());
                int vDead = 0, vFixed = 0, vAnchored = 0;
                double vMaxDy = 0.0;
                std::vector<uint8_t> seq;
                // Replay from the checkpoint of the previous cross-check point. This only
                // moves the replay origin closer; the input sequence replayed is unchanged
                // (equivalence is decided by the input sequence). Only branches without an
                // ancestor checkpoint replay from the section head.
                std::vector<int> anc((size_t)nxt.size(), -1);
                std::unordered_map<int, CheckpointObject*> newAnchors;
                if (g_anchor) {
                    for (size_t k = 0; k < nxt.size(); ++k) {
                        int a = nxt[k];
                        for (int s = 0; s < g_verifyEvery && a > 0; ++s)
                            a = g_nodes[(size_t)a].parent;
                        if (a > 0 && g_anchors.count(a)) anc[k] = a;
                    }
                }
                for (size_t k = 0; k < nxt.size(); ++k) {
                    const int ni = nxt[k];
                    const int a = g_anchor ? anc[k] : -1;
                    seq.clear();
                    for (int i = ni; i > 0 && i != a; i = g_nodes[(size_t)i].parent)
                        seq.push_back(g_nodes[(size_t)i].in);
                    std::reverse(seq.begin(), seq.end());
                    if (a > 0) {
                        secRestoreFrom(pl, g_anchors[a], g_nodes[(size_t)a].in);
                        ++restores; ++vAnchored;
                        for (int f = 0; f < freeze; ++f) {
                            secStep(g_nodes[(size_t)a].in, dt); ++steps;
                        }
                    } else {
                        secRestoreFrom(pl, g_ckpt, g_headHeld); ++restores;
                        for (int f = 0; f < freeze; ++f) {
                            secStep(g_headHeld, dt); ++steps;
                        }
                    }
                    bool dead = false;
                    for (uint8_t in : seq) {
                        g_died = false;
                        secStep((int)in, dt); ++steps;
                        if (!m_player1 || m_player1->m_isDead || g_died) {
                            dead = true; break;
                        }
                    }
                    if (dead) { ++vDead; releaseCp(ni); continue; }
                    auto* vp = m_player1;
                    const double ry = vp->getPositionY();
                    const double rv = (double)vp->m_yVelocity;
                    const double dy = std::fabs(ry - (double)g_nodes[(size_t)ni].y);
                    const double dv = std::fabs(rv - (double)g_nodes[(size_t)ni].vy);
                    vMaxDy = std::max(vMaxDy, std::max(dy, dv));
                    if (dy > g_verifyTol || dv > g_verifyTol) ++vFixed;
                    g_nodes[(size_t)ni].y = (float)ry;
                    g_nodes[(size_t)ni].vy = (float)rv;
                    // Retake from the real state. Skipping this means the match passed but
                    // the continuation grows from the old snapshot.
                    psnap::capture(vp, this, snaps[(size_t)ni]);
                    psnap::captureEM(this, pulses[(size_t)ni]);
                    psnap::captureState(this, states[(size_t)ni]);
                    psnap::captureTouch(vp, touches[(size_t)ni]);
                    if (g_worldOn)
                        psnap::captureWorld(g_movSet, worlds[(size_t)ni]);
                    // Origin of the next cross-check. The real state is here right now, so
                    // make it here.
                    if (g_anchor) {
                        if (CheckpointObject* ac = pl->markCheckpoint()) {
                            ac->retain();
                            newAnchors[ni] = ac;
                        }
                    }
                    ok.push_back(ni);
                }
                // Discard and replace the old generation of anchors. Letting them pile up
                // stacks 0.95MB x cap per generation. Release them reliably here.
                for (auto& kv : g_anchors) if (kv.second) kv.second->release();
                g_anchors.swap(newAnchors);
                // A measurement of how much psnap lied. The prior gates (sweep, moving
                // objects, moving portals) are all just guesses at "may it be used", and
                // lv22's ball section passed through every one of them and produced
                // maxDiff 14.4px. The caller can look at this number and re-solve with the
                // checkpoint.
                maxVerifyDrift = std::max(maxVerifyDrift, vMaxDy);
                char vb[240];
                snprintf(vb, sizeof(vb),
                         "secfix: d=%d checked=%zu dead=%d drift=%d maxDiff=%.4f "
                         "kept=%zu anchored=%d",
                         depth, nxt.size(), vDead, vFixed, vMaxDy, ok.size(),
                         vAnchored);
                writeResult(vb);
                nxt.swap(ok);
                if (nxt.empty()) { deepest = depth; break; }
            }
            deepest = depth;
            if (g_log) {
                // Layer fingerprint. For directly reading, from one run of each, at which
                // layer the checkpoint path and the psnap path first split. Comparing
                // solutions cannot locate it -- it depends on which leaf crossed the goal
                // first.
                uint64_t fp = 1469598103934665603ull;
                for (int ni : nxt) {
                    const auto& nd = g_nodes[(size_t)ni];
                    const uint8_t* b = (const uint8_t*)&nd.y;
                    for (size_t j = 0; j < sizeof(float) * 2; ++j) {
                        fp ^= b[j]; fp *= 1099511628211ull;
                    }
                    fp ^= nd.in; fp *= 1099511628211ull;
                }
                int cntLo = 65535, cntHi = 0;
                for (int ni : nxt) {
                    const int c = (int)g_nodes[(size_t)ni].cnt;
                    cntLo = std::min(cntLo, c);
                    cntHi = std::max(cntHi, c);
                }
                if (cntLo > cntHi) { cntLo = 0; cntHi = 0; }
                char lb[288];
                snprintf(lb, sizeof(lb),
                         "seclayer: d=%d parents=%zu keep=%zu dead=%d dup=%d "
                         "capped=%d x=%.1f y=%.1f..%.1f deadX=%.1f fp=%016llx "
                         "xr=%.1f..%.1f axis=%c cnt=%d..%d",
                         depth, cur.size(), nxt.size(), layDead, layDup, layCap,
                         layMaxX, layMinY > 1e8 ? 0.0 : layMinY,
                         layMaxY < -1e8 ? 0.0 : layMaxY, deadMaxX,
                         (unsigned long long)fp,
                         layMinX > 1e8 ? 0.0 : layMinX, layMaxX,
                         capByX ? 'x' : 'y', cntLo, cntHi);
                writeResult(lb);
            }
            for (int ni : cur) releaseCp(ni);          // the previous layer is no longer needed
            if (nxt.empty()) break;
            cur.swap(nxt);
            // ---- hand the frame back (see secsolve::SecTask) ----
            // At the layer boundary and nowhere else: `cur` is the layer just finished, every
            // checkpoint of the previous one has been released, and the world is whatever the
            // last expansion's restore left -- the same state the next expansion would restore
            // over anyway. The HUD is written first so the frame that follows draws the
            // progress rather than the phase this started in.
            g_frontierNow = cur.size();
            if (g_sliceMs > 0) {
                char hb[96];
                snprintf(hb, sizeof(hb), "secsolve: layer %d/%d, frontier %zu",
                         depth, g_horizon, cur.size());
                g_hudPhase = hb;
                if (sliceExpired()) co_await std::suspend_always{};
            }
        }
        for (int ni : cur) releaseCp(ni);
        releaseAnchors();

        // The leaf cross-check (plain replay + exit survivability) is already done inside
        // the search (`evalLeaf`). Back when it ran once here, a dead-end first leaf to
        // reach the goal ended the search on the spot.

        g_active = false;
        g_noKill = false;
        g_tick = savedTick;
        g_nextInput = savedInput;
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        // 512. 256 is not enough. When movSet= was added the buffer stayed 256 and the
        // line was cut just before foundVy. The caller's regex requires everything up to
        // foundVy, so the match failed, and a solution that passed the cross-check was
        // discarded as "no verdict" and re-solved with the checkpoint (2026-08-08; burned
        // an hour of lv20). When adding long lines, check the buffer too.
        char b[640];
        snprintf(b, sizeof(b),
                 "secsolve: %s prim=%s movSet=%zu depth=%d/%d frontier=%zu restores=%lld "
                 "steps=%lld cps=%lld cpPeak=%lld freeze=%d ms=%.0f "
                 "rephases=%lld maskShrinks=%d ckptTick=%lld inputBase=%lld targetX=%.1f "
                 "foundX=%.1f foundTick=%lld foundY=%.3f foundVy=%.3f "
                 "targetY=%.1f targetYDir=%d targetDepth=%d grace=%d/%d "
                 "doomedLeaves=%lld killAboveY=%.0f psnapDrift=%.2f",
                 // A solution that failed the cross-check is never called SOLVED. The
                 // caller looks only at the verdict, so rejecting it here makes it
                 // structurally impossible for a false solution to be grafted.
                 // A solution whose exit is a dead end is never called SOLVED (DOOMED).
                 // Even if it reaches the goal, it is no solution if every press from
                 // there dies. Dead-end exits are discarded and the search continues, so
                 // DOOMED appears when "paths to the goal were found, but every one was a
                 // dead end".
                 foundLeaf < 0 ? (doomedLeaves > 0 ? "DOOMED" : "EXHAUSTED")
                               : (g_leafVerified ? "SOLVED" : "UNVERIFIED"),
                 !g_snapOn ? "cp" : (g_worldOn ? "psnap+wsnap" : "psnap"),
                 g_worldOn ? g_movSet.size() : (size_t)0, deepest, g_horizon,
                 cur.size(), restores, steps, cpMade, cpPeak, freeze, ms,
                 rephases, psnap::g_shrinks, (long long)g_ckptTick,
                 // The absolute-tick origin when grafting onto the plain replay =
                 // ckptTick itself. State and input each count differently by one, so it
                 // was decided by measurement, not derivation:
                 //   state: the restore lands on tick ckptTick+1; depth d = tick
                 //          ckptTick+1+d
                 //   input: the branch input at depth d is the plain run's at tick
                 //          ckptTick+d
                 // (the shape of a button taking 2 substeps to reach the physics. Theory
                 //  can tip either way, so it was measured twice independently and agreed:
                 //  the secoff sweep of secverify gave off=-2, the graft-position sweep of
                 //  a SOLVED solution gave base=ckptTick, and in both y/vy matched the
                 //  plain run exactly.)
                 // Never confirm by survival: where the band is wide, several shifts pass
                 // (in lv20 both -2 and -3 survived). Match on foundY/foundVy.
                 (long long)g_ckptTick, g_targetX,
                 g_leafVerified ? verifyX : foundX,
                 // Which tick of the plain run the exit state corresponds to. The state at
                 // depth d is tick ckptTick+1+d. Do not make the caller re-count (today's
                 // lesson).
                 foundLeaf >= 0 ? (long long)(g_ckptTick + 1 + foundDepth) : -1LL,
                 // The seam expectation must be the replay's measured value. The value the
                 // search holds can be off under psnap (measured dy=8.2px). The caller
                 // uses this as check_seam's expectation, so passing a lie breaks the seam
                 // and becomes indistinguishable from a fidelity problem.
                 g_leafVerified ? verifyY
                                : (foundLeaf >= 0
                                   ? (double)g_nodes[(size_t)foundLeaf].y : 0.0),
                 g_leafVerified ? verifyVy
                                : (foundLeaf >= 0
                                   ? (double)g_nodes[(size_t)foundLeaf].vy : 0.0),
                 g_targetY, g_targetYDir, g_targetDepth, graceOk, graceDeadAt,
                 doomedLeaves, killAboveY, maxVerifyDrift);
        writeResult(b);
        if (foundLeaf >= 0) {
            char vb[280];
            snprintf(vb, sizeof(vb),
                     "secsolve_verify: %s deadAt=%d x=%.1f y=%.3f vy=%.3f "
                     "dy=%.4f dvy=%.4f dx=%.4f",
                     g_leafVerified ? "OK" : "FAIL", g_leafDeadAt, verifyX,
                     verifyY, verifyVy,
                     verifyY - (double)g_nodes[(size_t)foundLeaf].y,
                     verifyVy - (double)g_nodes[(size_t)foundLeaf].vy,
                     verifyX - (double)g_nodes[(size_t)foundLeaf].x);
            writeResult(vb);
            // The first depth at which the search's transitions and the plain replay
            // split. Unless this is -1, the section solver's premise ("checkpoint
            // branching = plain replay") is broken.
            char sb[280];
            snprintf(sb, sizeof(sb),
                     "secsolve_split: depth=%d/%d x %.3f->%.3f y %.3f->%.3f "
                     "vy %.3f->%.3f",
                     splitAt, foundDepth, splitNodeX, splitRepX,
                     splitNodeY, splitRepY, splitNodeVy, splitRepVy);
            writeResult(sb);
        }
        if (foundLeaf >= 0)
            writeResult("secsolve_inputs: " + inputsOf(foundLeaf));
        // Go through the official end path. Just setting g_sessionOver directly does not
        // honor quitwhendone; the worker lingers and the caller times out
        endSession("secsolve");
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        ++g_pcCalls;   // sole direct indicator that physics runs (goes to the heartbeat)
        // Tick counter: this substep executes as number g_tick
        // Input injection: call handleButton directly before the target tick's substep
        // runs (exact injection in tick space, independent of the queue's
        // m_step/timestamp semantics)
        // While the section solver is running, it holds the button one tick at a time.
        // The plan's input feed is stopped (a restore resets g_tick and g_nextInput to 0,
        // so mixing them would replay the plan again from the section head).
        if (secsolve::g_active) {
            if (secsolve::g_feed != secsolve::g_held) {
                g_injecting = true;
                this->handleButton(secsolve::g_feed != 0, 1, true);
                g_injecting = false;
                secsolve::g_held = secsolve::g_feed;
            }
        } else if (g_started && !g_sessionOver) {
            while (g_nextInput < g_cfg.inputs.size()
                   && g_cfg.inputs[g_nextInput].step <= g_tick) {
                auto& in = g_cfg.inputs[g_nextInput];
                ev("INJECT_handleButton", in.step, in.down);
                g_injecting = true;
                this->handleButton(in.down, 1 /*jump*/, true /*player1*/);
                g_injecting = false;
                if (in.down) orbtrace::g_lastPress = g_tick;
                ++solver::g_injThisAttempt;
                ++g_nextInput;
            }
            while (g_nextToggle < g_cfg.toggles.size()
                   && g_cfg.toggles[g_nextToggle].step <= g_tick) {
                auto& tg = g_cfg.toggles[g_nextToggle];
                ev("INJECT_toggle", tg.step);
                auto* p = m_player1;
                if (p) {
                    if (tg.mode == "ship") p->toggleFlyMode(true, true);
                    else if (tg.mode == "ball") p->toggleRollMode(true, true);
                    else if (tg.mode == "ufo") p->toggleBirdMode(true, true);
                    else if (tg.mode == "wave") p->toggleDartMode(true, true);
                    else if (tg.mode == "robot") p->toggleRobotMode(true, true);
                    else if (tg.mode == "spider") p->toggleSpiderMode(true, true);
                    else if (tg.mode == "swing") p->toggleSwingMode(true, true);
                    else if (tg.mode == "cube") {
                        p->toggleFlyMode(false, true);
                    }
                }
                ++g_nextToggle;
            }
        }
        ev("processCommands", dt, isHalfTick, isLastTick);
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        // Only the active PlayLayer advances the session bookkeeping. A scene swap takes
        // effect next frame, so within the same frame the old layer's remaining updates
        // still run and would dirty g_tick / the input cursor
        if (static_cast<GJBaseGameLayer*>(PlayLayer::get()) != this) return;
        ++g_tick;
        // Real positions of moving geometry (cfg `grouptrace=1`). This sits right after
        // ++g_tick so it gets the same tick numbers as dump/trace -- the model matches the
        // two on the same clock
        if (grouptrace::g_on && g_started && !g_sessionOver)
            grouptrace::tick(g_tick);
        // The player's own hitbox rect (cfg `hitboxtrace=1`, pbox line). The
        // collidedWithObject side only shows "the tick something was touched", so emit one
        // line whenever mode/size changes, checkable even without a hit (a few dozen lines
        // per level)
        if (g_cfg.hitboxTrace && g_started && !g_sessionOver && m_player1) {
            static int lastMode = -1;
            static float lastSize = -1.f;
            const int md = m_player1->m_isShip ? 1 : m_player1->m_isBall ? 2
                         : m_player1->m_isBird ? 3 : m_player1->m_isDart ? 4
                         : m_player1->m_isRobot ? 5 : m_player1->m_isSpider ? 6
                         : m_player1->m_isSwing ? 7 : 0;
            if (md != lastMode || m_player1->m_vehicleSize != lastSize) {
                lastMode = md; lastSize = m_player1->m_vehicleSize;
                CCRect pr = m_player1->getObjectRect();
                char b[192];
                snprintf(b, sizeof(b),
                    "pbox: t=%lld mode=%d size=%.3f rect=(%.3f,%.3f,%.3f,%.3f)",
                    (long long)g_tick, md, m_player1->m_vehicleSize,
                    pr.origin.x, pr.origin.y, pr.size.width, pr.size.height);
                writeResult(b);
            }
        }
        // Have GD itself enumerate "what could kill the player right now"
        // (cfg `hitboxtrace=1` + the hbfrom/hbto window, `dmg:` line).
        //
        // Why it is needed: searching objrects around the death spot does not work for
        // objects that were moved (static cx,cy stay as at entry;
        // gd-locked-object-position-lies). grouptrace also does not track m_objectType
        // 7/20/22/31, so a culprit there shows up in neither. GD's damagingObjectsInRect
        // pulls by section and does not filter by type -- i.e. this is the only ground
        // truth table.
        if (g_cfg.hitboxTrace && g_started && !g_sessionOver && m_player1
            && g_tick >= g_cfg.hbFrom
            && (g_cfg.hbTo == 0 || g_tick <= g_cfg.hbTo)) {
            const CCRect pr = m_player1->getObjectRect();
            CCRect q = pr;
            q.origin.x -= 30.f; q.origin.y -= 30.f;
            q.size.width += 60.f; q.size.height += 60.f;
            // enabledGroups=true. With false, grouped objects drop out entirely -- that is
            // why lv22 x=16,165 looked like "0 solids", while the real culprit was the
            // solid block with groups=1 (uid 6271).
            CCArray* a = this->damagingObjectsInRect(q, true);
            {
                char h[160];
                snprintf(h, sizeof(h),
                    "dmgn: t=%lld n=%d q=(%.1f,%.1f,%.1f,%.1f)",
                    (long long)g_tick, a ? (int)a->count() : -1,
                    q.origin.x, q.origin.y, q.size.width, q.size.height);
                writeResult(h);
            }
            // Query the solid side with the same question. Spawned objects and objects
            // excluded by type are in neither objrects nor grouptrace, so only the section
            // pull is true.
            if (CCArray* s = this->staticObjectsInRect(q, true)) {
                for (auto* o : CCArrayExt<GameObject*>(s)) {
                    if (!o) continue;
                    const CCRect r = o->getObjectRect();
                    char b[224];
                    snprintf(b, sizeof(b),
                        "stat: t=%lld uid=%d id=%d type=%d rect=(%.3f,%.3f,%.3f,%.3f)",
                        (long long)g_tick, (int)o->m_uniqueID, (int)o->m_objectID,
                        (int)o->m_objectType, r.origin.x, r.origin.y,
                        r.size.width, r.size.height);
                    writeResult(b);
                }
            }
            if (a) {
                for (auto* o : CCArrayExt<GameObject*>(a)) {
                    if (!o) continue;
                    const CCRect r = o->getObjectRect();
                    char b[224];
                    snprintf(b, sizeof(b),
                        "dmg: t=%lld uid=%d id=%d type=%d rect=(%.3f,%.3f,%.3f,%.3f)"
                        " prect=(%.3f,%.3f,%.3f,%.3f)",
                        (long long)g_tick, (int)o->m_uniqueID, (int)o->m_objectID,
                        (int)o->m_objectType, r.origin.x, r.origin.y,
                        r.size.width, r.size.height,
                        pr.origin.x, pr.origin.y, pr.size.width, pr.size.height);
                    writeResult(b);
                }
            }
        }
        // The player's oriented hitbox (cfg `hitboxtrace=1`, pobb line, every tick).
        //
        // Why the rect alone is not enough: the hazard test read from the disassembly is a
        // dedicated loop inside GJBaseGameLayer::checkCollisions (win 0x2137f0):
        //
        //   1. AABB: player->getObjectRect() vs the other's rect (strict comparison)
        //   2. if the other has m_shouldUseOuterOb, OBB-vs-OBB overlap
        //   3. if it passes, this->destroyPlayer(player, obj)
        //
        // a two-stage setup. collidedWithObject is never taken (measured: not one
        // hbox/hbin line on the lethal tick). Stage 1 is visible via pbox, but the
        // player-side box of stage 2 appeared nowhere. OBB2D is assembled by
        // GameObject::updateOrientedBox from m_width*..*scaleX / m_height*..*scaleY and
        // the rotation, so emitting the real 4 corners is faster and surer than guessing
        // the dimensions from outside.
        if (g_cfg.hitboxTrace && g_started && !g_sessionOver && m_player1) {
            static int pobbLines = 0;
            if (++pobbLines <= 40000) {
                if (OBB2D* ob = m_player1->getOrientedBox()) {
                    const auto& c = ob->m_corners;
                    // The 3 flags that decide the rotation's sign + the speed.
                    // runNormalRotation (win 0x38d220), the real thing:
                    //   m_rotationSpeed = 180 * s(+0x9bf) * s(+0x9c2) * s(+0x9c3)
                    //                     * [+0xb84] * speedArg / k
                    //   k = 0.4333333 (player size 1.0) / 0.3333333 (otherwise)
                    // 180/0.4333333 = 415.3846 deg/s = 1.730769 deg/tick,
                    // 180/0.3333333 = 540 deg/s = 2.25 deg/tick -- exact match with
                    // measurement. The magnitude is closed, so only these 3 signs remain.
                    // Their names cannot be pulled from bindings, so emit raw bytes.
                    const char* pb = reinterpret_cast<const char*>(m_player1);
                    const int f1 = (int)(unsigned char)pb[0x9bf];
                    const int f2 = (int)(unsigned char)pb[0x9c2];
                    const int f3 = (int)(unsigned char)pb[0x9c3];
                    float rmul = 0.f, rspd = 0.f;
                    std::memcpy(&rmul, pb + 0xb84, sizeof(float));
                    std::memcpy(&rspd, pb + 0x720, sizeof(float));
                    char b[512];
                    snprintf(b, sizeof(b),
                        "pobb: t=%lld size=%.3f rot=%.3f "
                        "c=(%.3f,%.3f)(%.3f,%.3f)(%.3f,%.3f)(%.3f,%.3f) "
                        "f=%d,%d,%d mul=%.4f rspd=%.4f",
                        (long long)g_tick, m_player1->m_vehicleSize,
                        m_player1->getRotation(),
                        c[0].x, c[0].y, c[1].x, c[1].y,
                        c[2].x, c[2].y, c[3].x, c[3].y,
                        f1, f2, f3, rmul, rspd);
                    writeResult(b);
                }
            }
        }
        // Dump the PlayerObject flag region as raw bytes (cfg `hitboxtrace=1` +
        // the `hbfrom`/`hbto` window, `pflags:` line).
        //
        // Why it is needed: on 2026-08-19 we hit the contradiction that "the same state
        // (gravity-flipped ship, zero input, sp0.9, gravityMod=1, vsize=1, and pobb's
        // 0x9bf/0x9c2/0x9c3 identical too) takes different ship acceleration branches on
        // the calibration rig vs the corpus" (see findings). GD appears to have a second
        // gravity flag, but it has no name in bindings, so diffing raw bytes is the only
        // way. 0x9a0..0x9e0 is the band collidedWithObjectInternal uses for the mode test
        // (0x9b9 ship / 0x9ba UFO / 0x9bb ball / 0x9bc wave / 0x9bd robot /
        // 0x9be spider / 0x9c4 swing / 0x9bf gravity).
        if (g_cfg.hitboxTrace && g_started && !g_sessionOver && m_player1
            && g_tick >= g_cfg.hbFrom
            && (g_cfg.hbTo == 0 || g_tick <= g_cfg.hbTo)) {
            const char* pb = reinterpret_cast<const char*>(m_player1);
            // 0x600-0xa20 in chunks of 0x80 bytes (384 characters per line)
            for (int base = 0x600; base < 0xa20; base += 0x80) {
                char b[600];
                int n = snprintf(b, sizeof(b), "pflags: t=%lld o=%03x",
                                 (long long)g_tick, base);
                for (int off = base; off < base + 0x80
                                     && n < (int)sizeof(b) - 8; ++off)
                    n += snprintf(b + n, sizeof(b) - n, " %02x",
                                  (unsigned)(unsigned char)pb[off]);
                writeResult(b);
            }
        }
        // What the player is standing on (cfg `standtrace=1`, stand line).
        //
        // Emitted to name invisible surfaces. At lv20 x=6,342 GD lands the mini ship
        // (onGround=1 vy=0) and it then slides down at -1.01/tick to its death, yet the
        // surface is in neither objrects nor grouptrace (0 objects of type 0/2/25/47 in
        // x 6,320-6,390 / y 230-320, and 0 moving ones). The object does not exist at load
        // time, so instead of reverse-lookup by position, ask for the pointer GD holds.
        // Emit only on ticks where it changed (a few dozen lines per level).
        if (g_cfg.standTrace && g_started && !g_sessionOver && m_player1) {
            static int lastSnap = -2, lastSlope = -2, lastTgt = -2;
            auto* p = m_player1;
            GameObject* sn = p->m_objectSnappedTo;
            GameObject* sl = static_cast<GameObject*>(p->m_currentSlope);
            // The raw field holding the spider's teleport target.
            // Disassembly (2.208): the body of PlayerObject::spiderTestJump is at
            // 0x393ff0; inside it, 0x394200 accumulates the running maximum into
            // `[this+0x968]` with maxsd, and writes the object it will land on to
            // [this+0x5e8] (right after 0x39083B). m_objectSnappedTo stays stuck at
            // t=1,469 and is never updated, so the teleport target surface can only be
            // named by this raw field. 0x960/0x968 are the candidates' min/max.
            GameObject* tg =
                *reinterpret_cast<GameObject**>((char*)p + 0x5e8);
            const double tgLo =
                *reinterpret_cast<double*>((char*)p + 0x960);
            const double tgHi =
                *reinterpret_cast<double*>((char*)p + 0x968);
            const int snId = sn ? sn->m_uniqueID : -1;
            const int slId = sl ? sl->m_uniqueID : -1;
            const int tgId = tg ? tg->m_uniqueID : -1;
            if (snId != lastSnap || slId != lastSlope || tgId != lastTgt) {
                lastSnap = snId; lastSlope = slId; lastTgt = tgId;
                char b[352];
                auto pos = [](GameObject* o, char* dst, size_t n) {
                    if (!o) { snprintf(dst, n, "-"); return; }
                    auto q = o->getPosition();
                    auto r = o->getObjectRect();
                    snprintf(dst, n, "uid%d id%d type%d @%.2f,%.2f %.2fx%.2f",
                             o->m_uniqueID, o->m_objectID, (int)o->m_objectType,
                             q.x, q.y, r.size.width, r.size.height);
                };
                char sb[160], lb[160], tb[160];
                pos(sn, sb, sizeof(sb));
                pos(sl, lb, sizeof(lb));
                pos(tg, tb, sizeof(tb));
                char b2[560];
                snprintf(b2, sizeof(b2),
                         "stand: t=%lld y=%.3f onG=%d snap=[%s] slope=[%s] "
                         "tgt=[%s] lo=%.3f hi=%.3f",
                         (long long)g_tick, m_player1->getPositionY(),
                         (int)m_player1->m_isOnGround, sb, lb, tb, tgLo, tgHi);
                (void)b;
                writeResult(b2);
            }
        }
        // Detection of missed speed portals. Detection runs always (no side effects).
        // Going as far as rejection only happens with cfg `speedgate=1`
        if (g_started && !g_sessionOver && m_player1) {
            speedgate::build(this);
            speedgate::check(m_player1, g_tick);   // detect and report only (no side effects)
        }
        // Record speed change points (cfg `syncprobe=1`). If one was missed, the steps
        // shift from that point on
        if (g_syncProbe && g_started && !g_sessionOver && m_player1) {
            static float s_lastSpd = -1.f;
            float spd = (float)m_player1->m_playerSpeed;
            if (spd != s_lastSpd) {
                s_lastSpd = spd;
                char sp[160];
                snprintf(sp, sizeof(sp), "speed: %.2f at tick=%lld x=%.0f y=%.0f",
                    spd, (long long)g_tick, m_player1->getPositionX(),
                    m_player1->getPositionY());
                writeResult(sp);
            }
        }
        // Count idle ticks while dead (evidence for whether they can be skipped). One
        // line, so always collected
        if (g_started && !g_sessionOver && m_player1)
            deadstat::tick(m_player1->m_isDead);
        // Real positions of moving gates (cfg `gatetrace`). Where a moving block is at any
        // time can only be seen from real positions
        if (solver::g_gateTrace && !solver::g_gateObjs.empty()
            && g_tick >= solver::g_gateT0 && g_tick <= solver::g_gateT1
            && (solver::g_gateStride <= 1 || (g_tick % solver::g_gateStride) == 0)) {
            if (!solver::g_gateHdrDone) {
                solver::g_gateHdrDone = true;
                for (auto* o : solver::g_gateObjs) {
                    if (!o) continue;
                    char hb[160];
                    auto r = o->getObjectRect();
                    snprintf(hb, sizeof(hb),
                        "gateobj: id=%d uid=%d x=%.2f y=%.2f rect=%.2f,%.2f,%.2f,%.2f",
                        o->m_objectID, o->m_uniqueID,
                        o->getPositionX(), o->getPositionY(),
                        r.origin.x, r.origin.y, r.size.width, r.size.height);
                    writeResult(hb);
                }
            }
            std::string s = "gate: tick=" + std::to_string(g_tick);
            if (m_player1) {
                char pb[224];
                // Emit the real hitbox and GD's own collision record (lcol)
                auto pr = m_player1->getObjectRect();
                auto* g = m_player1->m_maybeLastGroundObject;
                snprintf(pb, sizeof(pb),
                    " px=%.1f py=%.1f prect=%.2f,%.2f,%.2f,%.2f ground=%d@%.0f,%.0f"
                    " lcol=%d,%d,%d,%d lk=%d%d",
                    m_player1->getPositionX(), m_player1->getPositionY(),
                    pr.origin.x, pr.origin.y, pr.size.width, pr.size.height,
                    g ? g->m_objectID : -1,
                    g ? g->getPositionX() : 0.f, g ? g->getPositionY() : 0.f,
                    m_player1->m_lastCollisionBottom, m_player1->m_lastCollisionTop,
                    m_player1->m_lastCollisionLeft, m_player1->m_lastCollisionRight,
                    (int)m_player1->m_isLocked, (int)m_player1->m_controlsDisabled);
                s += pb;
            }
            for (auto* o : solver::g_gateObjs) {
                if (!o) continue;
                char b[96];
                // Also emit the effective hitbox state (even with the position fixed, a
                // toggle can remove the hitbox).
                // Flag sequence = m_isDisabled, m_isHide, visible, m_isGroupDisabled,
                // m_isGroupDisabledTemp, m_hasExtendedCollision, m_isPassable
                snprintf(b, sizeof(b), " [%d %.0f,%.0f %d%d%d%d%d%d%d]",
                    o->m_objectID, o->getPositionX(), o->getPositionY(),
                    (int)o->m_isDisabled, (int)o->m_isHide, (int)o->isVisible(),
                    (int)o->m_isGroupDisabled, (int)o->m_isGroupDisabledTemp,
                    (int)o->m_hasExtendedCollision, (int)o->m_isPassable);
                s += b;
            }
            writeResult(s);
        }
        // State injection probe (cfg `colprobe`, explained in solver.hpp). Injection
        // happens after tick T's physics update, so it takes effect from the T+1
        // transition
        if (solver::g_colProbe && m_player1 && !g_sessionOver) {
            if (g_tick == solver::g_cpT && !solver::g_cpInjected
                && !m_player1->m_isDead) {
                solver::g_cpInjected = true;
                const double y = solver::g_cpY0
                    + (double)(g_attempt - 1) * solver::g_cpYStep;
                m_player1->setPositionY((float)y);
                m_player1->m_yVelocity = solver::g_cpVy;
                solver::g_cpEndTick = g_tick + solver::g_cpN;
                char ib[128];
                snprintf(ib, sizeof(ib), "cprobe: inject a=%d t=%lld x=%.2f y=%.3f vy=%.3f",
                    g_attempt, (long long)g_tick, m_player1->getPositionX(),
                    y, solver::g_cpVy);
                writeResult(ib);
            }
            if (solver::g_cpInjected && g_tick > solver::g_cpT
                && g_tick <= solver::g_cpEndTick) {
                char cb[176];
                snprintf(cb, sizeof(cb),
                    "cprobe: a=%d t=%lld x=%.2f y=%.3f vy=%.3f dead=%d lcol=%d,%d,%d,%d",
                    g_attempt, (long long)g_tick,
                    m_player1->getPositionX(), m_player1->getPositionY(),
                    (double)m_player1->m_yVelocity, (int)m_player1->m_isDead,
                    m_player1->m_lastCollisionBottom, m_player1->m_lastCollisionTop,
                    m_player1->m_lastCollisionLeft, m_player1->m_lastCollisionRight);
                writeResult(cb);
                if (g_tick == solver::g_cpEndTick && !m_player1->m_isDead) {
                    // Observation done. If still alive, kill and advance to the next attempt
                    if (auto* pl = PlayLayer::get())
                        pl->destroyPlayer(m_player1, m_player1);
                }
            }
        }
        // State injection and cutoff for analysis (`inject=` / `stopat=` in plan_in.txt).
        // Injection happens after tick T's physics update, so it takes effect from the
        // T+1 transition. A tool to remove the need to "first solve a plan that reaches
        // that state"
        if (m_player1 && !g_sessionOver && !m_player1->m_isDead) {
            // Discard injections whose tick was skipped (T was jumped over by a checkpoint
            // restore etc.). If kept, they suddenly fire at a much later tick and look
            // like an unexplained branch
            while (g_injectNext < g_injects.size()
                   && g_injects[g_injectNext].tick < g_tick) ++g_injectNext;
            while (g_injectNext < g_injects.size()
                   && g_injects[g_injectNext].tick == g_tick) {
                auto& s = g_injects[g_injectNext++];
                if (s.hasX)  m_player1->setPositionX((float)s.x);
                if (s.hasY)  m_player1->setPositionY((float)s.y);
                if (s.hasVy) m_player1->m_yVelocity = s.vy;
                char ib[192];
                snprintf(ib, sizeof(ib),
                    "inject: t=%lld x=%.3f y=%.3f vy=%.3f mode=%s",
                    (long long)g_tick, m_player1->getPositionX(),
                    m_player1->getPositionY(), (double)m_player1->m_yVelocity,
                    modeStr(m_player1));
                writeResult(ib);
            }
            // Cutoff: report the state, then kill. The report must come first --
            // destroyPlayer emits a death: line, and in the reverse order the caller
            // cannot distinguish "cutoff" from "a real death"
            if (g_stopAt >= 0 && g_tick == g_stopAt && !g_stopFired) {
                char sb[256];
                snprintf(sb, sizeof(sb),
                    "stop: attempt=%d tick=%lld x=%.3f y=%.3f yvel=%.3f mode=%s "
                    "onGround=%d vsize=%.3f",
                    g_attempt, (long long)g_tick, m_player1->getPositionX(),
                    m_player1->getPositionY(), (double)m_player1->m_yVelocity,
                    modeStr(m_player1), (int)m_player1->m_isOnGround,
                    (double)m_player1->m_vehicleSize);
                writeResult(sb);
                g_stopFired = true;   // never twice per attempt (the directive itself is kept)
                if (auto* pl = PlayLayer::get())
                    pl->destroyPlayer(m_player1, m_player1);
            }
        }
        // Observing the trigger of automatic checkpoint placement (cfg `ckpttrace=1`).
        // Catches at tick resolution the moment GD decides it "wants to place one"
        // (m_shouldTryPlacingCheckpoint). The actual creation/storage is picked up by the
        // PlayLayer-side hook, so this is for transitions only
        if (solver::ckpttrace::g_on && g_started && !g_sessionOver && m_player1) {
            int tryNow = m_player1->m_shouldTryPlacingCheckpoint ? 1 : 0;
            int toNow  = m_player1->m_checkpointTimeout ? 1 : 0;
            if (tryNow != solver::ckpttrace::g_lastTry
                || toNow != solver::ckpttrace::g_lastTimeout) {
                // Do not flood result.txt if transitions turn out to happen every tick.
                // Always say the cap was hit (cutting silently looks like "nothing
                // happened after that")
                if (++solver::ckpttrace::g_flagLines > solver::ckpttrace::kFlagCap) {
                    if (solver::ckpttrace::g_flagLines == solver::ckpttrace::kFlagCap + 1)
                        writeResult("ckpt: flag   (capped at "
                            + std::to_string(solver::ckpttrace::kFlagCap)
                            + " lines; further transitions not logged)");
                    solver::ckpttrace::g_lastTry = tryNow;
                    solver::ckpttrace::g_lastTimeout = toNow;
                } else {
                char cb[224];
                snprintf(cb, sizeof(cb),
                    "ckpt: flag   tick=%lld x=%.2f try=%d->%d timeout=%d->%d "
                    "lastCkptT=%.6f totalT=%.6f",
                    (long long)g_tick, m_player1->getPositionX(),
                    solver::ckpttrace::g_lastTry, tryNow,
                    solver::ckpttrace::g_lastTimeout, toNow,
                    m_player1->m_lastCheckpointTime, m_player1->m_totalTime);
                writeResult(cb);
                solver::ckpttrace::g_lastTry = tryNow;
                solver::ckpttrace::g_lastTimeout = toNow;
                }
            }
        }
        // In-memory per-tick record of grounded/mode/coords (used by diag's traj and
        // clearance::observe). Always recorded during a session. g_started/!g_sessionOver
        // are required: without them the per-tick recording keeps running in normal play
        // after the session ends
        if (g_started && !g_sessionOver && m_player1) {
            if ((long long)solver::g_log.size() <= g_tick)
                solver::g_log.resize(g_tick + 1);
            // In dual, record p2's grounded state too (one input moves both bodies, so a
            // press is meaningful if p2 is grounded even while p1 is airborne)
            bool dual = m_gameState.m_isDualMode;
            // Have it say once whether the flag is really set (so it is visible from
            // outside that it is in effect)
            if (dual && !solver::g_dualSeen) {
                solver::g_dualSeen = true;
                writeResult("dual: entered at tick=" + std::to_string((long long)g_tick)
                    + " x=" + std::to_string((int)m_player1->getPositionX())
                    + " p2=" + (m_player2 ? "yes" : "NO"));
            }
            solver::g_log[g_tick] = {
                m_player1->getPositionX(),
                m_player1->getPositionY(),
                (uint8_t)(m_player1->m_isOnGround ? 1 : 0),
                solver::modeOf(m_player1),
                (uint8_t)(m_player1->m_isOnGround2 ? 1 : 0),
                (float)m_player1->m_yVelocity,
                (uint8_t)(dual ? 1 : 0),
                (uint8_t)((dual && m_player2 && m_player2->m_isOnGround) ? 1 : 0)
            };
        }
        // 10-second heartbeat for plain replay. A plain replay writes nothing to
        // result.txt until it finishes, so this keeps the outer freeze detector
        // (no-update guard) from wrongly killing a healthy verification run
        if (g_started && !g_sessionOver && m_player1) {
            static auto s_last = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - s_last).count() >= 10) {
                s_last = now;
                writeResult("replay: t=" + std::to_string(g_tick)
                    + " x=" + std::to_string(m_player1->getPositionX())
                    + " y=" + std::to_string(m_player1->getPositionY()));
            }
        }
        // ...and a STALL guard on the same gate. A plan that leaves the player wedged
        // never ends its attempt. GD has no timeout of its own, so the fast loop spins
        // on a run that is already over and eats the rest of the budget. Measured on
        // lv22 (2026-08-24): a plan whose tail left the player stuck in a rotated frame
        // held BOTH coordinates at (20115.000000, 829.285767) to the last digit for 6.9
        // MILLION ticks and was still going when the session was killed -- one iteration
        // took the remaining 11 minutes of a 25-minute run. offBoardTick cannot see it:
        // the player is on the board, it just is not moving.
        //
        // Both coordinates have to stand still. The travel axis is x in an upright frame
        // and y in a turned one, so either one alone stands still legitimately and the
        // pair does not. The comparison is exact on purpose -- any motion at all, down
        // to the last bit, is a run that is still going.
        //
        // The threshold started at 30,000 ("only has to beat forever"), but the glitch
        // wedges turned out to be ROUTINE -- 76 of them in one run, each paying the whole
        // fuse in fast-loop time -- so it is now economical instead: 3,000 is 4.5x the
        // longest legitimate still-stand that ends a run in the corpus (668 ticks, lv22's
        // end-of-level pin). The
        // pause/freeze paths above return before g_tick advances, so a paused or
        // held-still session never accumulates any of it, and a new attempt (g_tick
        // going backwards) rearms it.
        // `!m_isDead`: a corpse is perfectly still through GD's death-to-reset delay,
        // and the fast loop pumps tens of thousands of ticks through that window --
        // counted, the guard re-fires on the corpse and every downstream tick number
        // (stall report, wedge credit, depth score) inherits the inflation.
        // !noDeath: the bootstrap recording pass (and any other nodeath observation
        // run) swallows its deaths and legitimately runs the whole level and beyond --
        // the overlong guard was killing it at 40k ticks forever, and the session never
        // got past attempt one.
        if (g_started && !g_sessionOver && m_player1 && !m_player1->m_isDead
            && !g_cfg.noDeath) {
            static float s_stallX = 0.f, s_stallY = 0.f;
            static long long s_stallSince = -1;
            // The END-ZONE pin is not a stall: GD locks the player at the end wall
            // while the scheduler-driven completion sequence plays, and under the fast
            // loop thousands of "stillness" ticks pass in a frame or two of real time.
            // The guards wait ENDZONE_KILL_AFTER_SEC of WALL time for a locked player
            // (measured: the fr110 lv11 solution was being stall-killed mid-clear at
            // x=29,503, and the armed completion then leaked into the next attempt as
            // an endscreen at t=0). A HUNG sequence -- the wrong-mode zombie that
            // never completes -- runs past the bound and is put down as before.
            // The lock clock is armed HERE, per tick, not only at the frame
            // boundary: one frame is fastloops x (fastdt*240) = 7,200 ticks, so
            // the pin and the 3,000-tick stillness both happen INSIDE one batch
            // and a boundary-only clock never sees the lock at all (measured:
            // the first build of this fix changed nothing on lv11).
            if (m_player1->m_isLocked && m_player1->getPositionX() > 500.f
                && g_endzoneLockStart.time_since_epoch().count() == 0)
                g_endzoneLockStart = std::chrono::steady_clock::now();
            const double lockWall =
                (m_player1->m_isLocked
                 && g_endzoneLockStart.time_since_epoch().count() != 0)
                    ? std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - g_endzoneLockStart).count()
                    : -1.0;
            const bool endzoneGrace = lockWall >= 0.0 && lockWall < ENDZONE_KILL_AFTER_SEC;
            // ...and once the grace has run out the sequence is hung, so a short
            // stillness suffices -- at 1 tick/frame the old 3,000 would cost most of a
            // minute of wall time per zombie attempt.
            const bool endzoneHung = lockWall >= ENDZONE_KILL_AFTER_SEC;
            // ...and an attempt that simply never ends. The MOVING form of the zombie
            // glitch bobs on a platform at 100%% forever -- no stillness, no death, no
            // completion -- and the fast loop pumped 1.7M ticks through one before
            // anything noticed. No legitimate attempt on these levels exceeds ~22k
            // ticks, so 40k is pure pathology.
            static int s_overFired = -1;   // once per attempt -- this runs per TICK, and
                                           // unlatched it wrote 1,800 lines per frame
            if (g_tick > 40000 && s_overFired != g_attempt && !endzoneGrace) {
                s_overFired = g_attempt;
                char ob2[160];
                snprintf(ob2, sizeof(ob2), "stall: attempt=%d has run %lld ticks without "
                         "ending - forcing it over", g_attempt, (long long)g_tick);
                writeResult(ob2);
                if (auto* pl = PlayLayer::get()) {
                    pl->destroyPlayer(m_player1, m_player1);
                    g_stallResetPending = true;
                }
            }
            const float px = m_player1->getPositionX();
            const float py = m_player1->getPositionY();
            if (s_stallSince < 0 || px != s_stallX || py != s_stallY
                || g_tick < s_stallSince) {
                s_stallX = px; s_stallY = py; s_stallSince = g_tick;
            } else if ((g_tick - s_stallSince >= 3000 && !endzoneGrace)
                       || (endzoneHung && g_tick - s_stallSince >= 300)) {
                char sb[256];
                snprintf(sb, sizeof(sb),
                    "stall: attempt=%d the player has not moved since t=%lld "
                    "(x=%.3f y=%.3f) - ending the attempt",
                    g_attempt, (long long)s_stallSince, (double)px, (double)py);
                writeResult(sb);
                s_stallSince = -1;
                if (auto* pl = PlayLayer::get()) {
                    pl->destroyPlayer(m_player1, m_player1);
                    // ...and destroyPlayer alone is NOT a guarantee, so a reset is
                    // forced as well. In the glitch state this guard exists for,
                    // the level believes itself finished and GD's destroy books the
                    // death without ever scheduling the restart: measured
                    // 2026-08-25, g_tick kept counting across "attempts" (57,751 ->
                    // 98,522 -> ..., +40,771 each) and the loop scored those
                    // inflated ticks as the deepest runs ever seen.
                    // DEFERRED to the frame boundary, not called here: a reset from
                    // inside the fast loop lets the rest of this frame's batch run
                    // on the fresh attempt with a stale input cursor -- measured as
                    // garbage deaths at t=660/1,837 on a prefix that had verified
                    // clean dozens of times, each one feeding false fixups.
                    g_stallResetPending = true;
                }
            }
        }
        // Measure the "uncontrollable boundary" of the end zone (cfg `endtrace=1`).
        // The completion flag (m_hasCompletedLevel) is set after the pull-in has made the
        // player uncontrollable = it is not the end of controllability, so measure which
        // flag drops first
        if (g_cfg.endTrace && g_started && !g_sessionOver && m_player1) {
            static long long s_lastTick = -1;
            static int s_prev = -1, s_lines = 0;
            if (g_tick <= s_lastTick) { s_prev = -1; s_lines = 0; } // the attempt changed
            s_lastTick = g_tick;
            auto* pl = PlayLayer::get();
            int lk = m_player1->m_isLocked ? 1 : 0;
            int co = m_player1->m_controlsDisabled ? 1 : 0;
            int cp = (pl && pl->m_hasCompletedLevel) ? 1 : 0;
            int dd = m_player1->m_isDead ? 1 : 0;
            int st = lk | (co << 1) | (cp << 2) | (dd << 3);
            if (st != s_prev && ++s_lines <= 40) {
                writeResult("endtrace: t=" + std::to_string(g_tick)
                    + " x=" + std::to_string(m_player1->getPositionX())
                    + " y=" + std::to_string(m_player1->getPositionY())
                    + " vy=" + std::to_string((float)m_player1->m_yVelocity)
                    + " locked=" + std::to_string(lk)
                    + " ctrlOff=" + std::to_string(co)
                    + " completed=" + std::to_string(cp)
                    + " dead=" + std::to_string(dd)
                    + " mode=" + modeStr(m_player1));
            }
            s_prev = st;
        }
        // Our own coin pickup test: a coordinate hit within a conservative radius counts
        // as picked up and the pickup tick is recorded (independent of GD's in-checkpoint
        // coin state. On a checkpoint restore, pickups after the restore tick are voided)
        if (g_started && !g_sessionOver && g_cfg.coinMode && m_player1) {
            float px = m_player1->getPositionX();
            float py = m_player1->getPositionY();
            // Pickups only during "normal motion" (see g_prevTickX in solver.hpp).
            // m_hasCompletedLevel alone cannot exclude the pull-in animation (it is set
            // too late), so the player-side uncontrollable flags
            // (locked / controlsDisabled) are also part of the condition
            float dxTick = px - solver::g_prevTickX;
            bool completed = false;
            if (auto* pl = PlayLayer::get()) completed = pl->m_hasCompletedLevel;
            bool uncontrolled = m_player1->m_isLocked || m_player1->m_controlsDisabled;
            bool normalMotion = solver::g_prevTickX > -1e8f
                && dxTick > 0.2f && dxTick < 5.0f && !completed && !uncontrolled;
            solver::g_prevTickX = px;
            for (size_t i = 0; i < solver::g_coins.size()
                 && normalMotion && i < solver::g_coinPickupTick.size(); ++i) {
                if (solver::g_coinPickupTick[i] >= 0) continue;
                if (std::abs(px - solver::g_coins[i].x) <= solver::COIN_RADIUS
                    && std::abs(py - solver::g_coins[i].y) <= solver::COIN_RADIUS)
                    solver::g_coinPickupTick[i] = g_tick;
            }
        }
        // End-of-tick record of what a re-anchor would need (Stage C). Same instant as the dump
        // row below and for the same reason -- the state has to be settled -- but deliberately
        // NOT behind `notrace`: the dump is a diagnostic, this is what the next iteration of the
        // repair loop resumes the search from.
        if (g_started && !g_sessionOver && g_cfg.dpSolve && m_player1)
            anchors::record(this, g_tick);
        // ...and the seek bar's own, much smaller record: one x per tick, which is what turns
        // "five seconds earlier" into a place on the bar. Not the anchors buffer, which is
        // twenty-odd fields wide and only exists during a solve.
        //
        // NOT AFTER THE SESSION ENDS. g_tick keeps counting for as long as the level is left
        // standing -- the results screen is not a stopped game, it is a game still being updated
        // once a frame -- and the bar's axis is the deepest tick it has seen. Reported 2026-08-28:
        // sit on the clear screen and the axis grows without limit, so the whole run's marks creep
        // leftwards and the playhead slides back off the end of a run that is over. The run ended
        // at the tick it ended at; that is where the bar stops.
        if (g_started && !g_sessionOver && m_player1)
            itermap::trackX(g_tick, m_player1->getPositionX());
        // GD's max-gameplay-y bound (layer+0x36a8, updateMaxGameplayY), logged
        // on change. On dynamic-height levels the bound moves with the world,
        // and whether it does -- and when -- decides how --maxplayy has to be
        // carried (one value vs a per-tick track like --bandtrack). A handful
        // of lines per attempt; not gated on dpsolve so a plain replay shows it.
        if (g_started && !g_sessionOver) {
            const float mp = *reinterpret_cast<float const*>(
                reinterpret_cast<char const*>(this) + 0x36a8);
            static float mpLast = -1.f;
            if (std::fabs(mp - mpLast) > 0.5f) {
                mpLast = mp;
                writeResult("maxplayy: t=" + std::to_string(g_tick)
                            + " v=" + std::to_string(mp));
            }
        }
        // End-of-tick state dump (after the player's physics is settled)
        if (g_started && !g_sessionOver
            && !g_cfg.noTrace && g_dump.is_open() && m_player1) {
            auto* p = m_player1;
            auto pos = p->getPosition();
            g_dump << g_frame << ',' << g_attempt << ',' << g_tick
                   << ',' << pos.x << ',' << pos.y
                   << ',' << p->m_yVelocity << ',' << p->getRotation()
                   << ',' << modeStr(p) << ',' << p->m_isUpsideDown
                   << ',' << p->m_isOnGround << ',' << p->m_isOnGround2
                   << ',' << p->m_isDead << ',' << p->m_playerSpeed
                   << ',' << p->m_gravityMod << ',' << p->m_platformerXVelocity
                   // vsize: the size portal (1.0=normal 0.6=mini). Needed for re-anchoring
                   << ',' << p->m_vehicleSize
                   // gy1/gy2: real positions of the ground/ceiling layers. They move, and
                   // they act as a standing surface even with no object there
                   // (getGroundY() returns 0, so emit the real positions)
                   << ',' << (this->m_groundLayer
                                  ? this->m_groundLayer->getPositionY() : -1.f)
                   << ',' << (this->m_groundLayer2
                                  ? this->m_groundLayer2->getPositionY() : -1.f)
                   // dual/p2*: the second body in dual (while not in dual, p2 is
                   // off-screen, so read it together with the dual flag)
                   << ',' << (m_gameState.m_isDualMode ? 1 : 0)
                   << ',' << (m_player2 ? m_player2->getPositionY() : -1.f)
                   << ',' << (m_player2 ? m_player2->m_yVelocity : 0.f)
                   << ',' << (m_player2 ? (int)m_player2->m_isUpsideDown : 0)
                   << ',' << (m_player2 ? (int)m_player2->m_isOnGround : 0)
                   << ',' << (m_player2 ? (int)m_player2->m_isDead : 0)
                   // pmin/pmax: GD's own flight band (getMinPortalY / getMaxPortalY).
                   //
                   // Emitted to remove the last x dependency of re-anchoring. When leveldp
                   // resumes with --start, it rebuilds the band from "the last mode portal
                   // before x0". That knows nothing about which lane was taken, so it also
                   // picks up portals that were never passed through. Measured (lv20
                   // 2026-08-09): the wave passes under the portal at (4445,303)
                   // (y=114..160; the box is y[253,353]), yet the model rewrote the band
                   // to [150,450], flew 34px too high from then on, and stopped dying
                   // where GD kills. fixup was grinding, buying back 4 ticks at a time.
                   // Emitting GD's values directly removes the guessing.
                   << ',' << this->getMinPortalY()
                   << ',' << this->getMaxPortalY()
                   // snapuid/snapdist: stair-snap state (the uid of m_objectSnappedTo and
                   // m_snapDistance). The last x that re-anchoring cannot carry.
                   //
                   // leveldp's --start resumes with snapObj=null, so at the first stair
                   // right after the anchor the pattern match fails (the gate needs "the
                   // object last stood on") and one snap is dropped. From then on that run
                   // trails GD by 1px forever.
                   // Measured (2026-08-10, the same 400-tick anchors as quick_regress,
                   // lv1-20): 64 sections split with dy=dvy=0 and dx=-1.0000 exactly, all
                   // on mode 0 surfaces. Every one is before the tick where the same
                   // level's "anchorless replay" splits = an artifact of the anchor, not
                   // physics. -1 = standing on nothing.
                   << ',' << (p->m_objectSnappedTo
                                  ? p->m_objectSnappedTo->m_uniqueID : -1)
                   << ',' << p->m_snapDistance
                   // camscale: the divisor [this+0x1a8] of getMaxPortalY (win 0x213770).
                   // The real thing is `getMinPortalY() + [this+0x2ec] / [this+0x1a8]`,
                   // where [+0x2ec] is the mode's band height. That is, the height of the
                   // invisible ceiling is divided by the camera scale. This is why, in
                   // lv22's swing section, pmax-pmin moves smoothly 324.000 -> 405.000
                   // (= height 300 divided by 0.92593 and 0.74074). Read by raw offset
                   // because bindings has no name for it
                   << ',' << *reinterpret_cast<const float*>(
                                 reinterpret_cast<const char*>(this) + 0x1a8)
                   // gframe: current rotation (0/1/2/3 = 0/90/180/270).
                   // Needed for re-anchoring. Starting with --start inside a rotated
                   // gameplay section, the model has no way to know which orientation it
                   // is running in (the rotate triggers count as already passed), resumes
                   // in frame 0 and falls off within 1 tick. lv22's anchor at t=11,340
                   // "followed for 1 tick" because of this.
                   << ',' << (int)g_gameFrame
                   // ctrlOff: m_controlsDisabled. When the Options trigger (id 2899) sets
                   // it, buttons are ignored entirely (no press, and no cube held
                   // re-jump either). lv22 has 4 such sections of 764/354/425/378 ticks,
                   // where only the model was jumping (fixcensus's
                   // m0/g1/gdg1/sp0.9/air/in1 family = edvy -11.180).
                   // Needed in the dump because a section anchor can start inside such a
                   // window (2899 is a one-shot on x crossing, so with --start inside the
                   // window the model has no way to know).
                   << ',' << (p->m_controlsDisabled ? 1 : 0)
                   // camx/camy: the camera (= the negated position of m_objectLayer).
                   // Emitted to measure the trigger firing gate. The 10 firings of id 2899
                   // cannot be explained by x crossing alone (there are passes over the
                   // same x that do not fire), nor by distance to the player's y
                   // (no fire at delta=1,236, fire at delta=1,324). GD wakes triggers when
                   // they enter the screen, so the remaining variable is the camera
                   // itself. Emit the raw values and verify the rule afterwards.
                   << ',' << (this->m_objectLayer
                                  ? -this->m_objectLayer->getPositionX() : 0.f)
                   << ',' << (this->m_objectLayer
                                  ? -this->m_objectLayer->getPositionY() : 0.f)
                   // p2ground2: the SECOND body's second contact flag, the partner of
                   // p2ground exactly as onGround2 is of onGround. GD's ground flag is
                   // sticky for the flying modes and the pair plus a still velocity is what
                   // actually says "resting" (see grounded_of / groundedOf). p1 has had both
                   // since the beginning; p2 had only one, so an anchor built from this dump
                   // could not apply the same rule to the second body.
                   // Appended at the END of the row on purpose -- readers index the earlier
                   // columns by position.
                   << ',' << (m_player2 ? (int)m_player2->m_isOnGround2 : 0)
                   // p2mode/p2vsize: THE SECOND BODY'S GAME MODE AND SIZE. Neither
                   // was emitted anywhere -- not here, not in leveldp's trace, not in
                   // `--start`. So the loop compared p2 on y and vy alone and called
                   // that agreement.
                   // [correction 2026-08-28] "while the model does carry mode2/mini2
                   // (swapHalves swaps them)" stood here and was HALF WRONG: State
                   // had mode2, but size was still on stepBoth's shared list, so the
                   // model had no second-body size to be told about. Both ends were
                   // built afterwards -- State::mini2, and the 26th/27th --start
                   // fields -- so the sentence is true now and was not then.
                   //
                   // That blind spot fits the corpus' largest grind exactly. lv16
                   // t=13,017 is hit 26 times: GD kills p2 twelve ticks before the
                   // model does and the fixup pass writes nothing, because "apart
                   // there by 0.00/0.00" is all it can see. GD names the killer --
                   // a 2.6x4.8 spike (uid 6563) at dy=-10.30 -- which needs an
                   // effective player half above 7.90 to reach: 9 (mini) or 15 does,
                   // the wave's hazard rect(5,3) does not. A mode the model has
                   // wrong is the first suspect, and lv16's dual span carries lone
                   // single-height portals (a ball at 19,219 / a ship at 20,499),
                   // which is precisely how the two halves come to differ.
                   //
                   // Emitted to TEST that, not on the strength of it. Appended at
                   // the end of the row like p2ground2: readers index by position.
                   //
                   // Measured as soon as the column existed: on lv16's solution the
                   // two bodies are in the same mode and size for all 5,557 dual
                   // ticks, so the anchor's assumption holds for the OFFICIAL corpus.
                   // It does not hold in general -- differing modes and sizes between
                   // the halves are ordinary in custom levels, which this solver
                   // already clears cold. So the assumption is a corpus artefact, and
                   // the column is what will show it the moment a custom level needs
                   // it.
                   << ',' << (m_player2 ? modeStr(m_player2) : "-")
                   << ',' << (m_player2 ? m_player2->m_vehicleSize : 0.f)
                   // p2x: and the same question one level down. The model gives the
                   // pair a SINGLE x (State::xAbs; swapHalves does not swap it), so
                   // two bodies at different x would not be representable at all
                   // rather than merely mis-anchored. Whether GD can produce that is
                   // not known, and guessing either way is worse than a column that
                   // answers it.
                   << ',' << (m_player2 ? m_player2->getPositionX() : -1.f)
                   << '\n';
        }
    }

    void processQueuedButtons(float dt, bool clearInputQueue) {
        ev("processQueuedButtons", dt, clearInputQueue);
        GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
    }

    // Record the fact that a switch (touch trigger) was touched (cfg `trigtrace=1`).
    // Which trigger was stepped on at which tick makes run lineages distinguishable
    void playerTouchedTrigger(PlayerObject* player, EffectGameObject* object) {
        if (g_trigTrace && g_started && !g_sessionOver && object
            && player == m_player1) {
            static int s_logs = 0;
            if (++s_logs <= 60) {
                auto p = object->getPosition();
                writeResult("trigger: id=" + std::to_string(object->m_objectID)
                    + " x=" + std::to_string((int)p.x)
                    + " y=" + std::to_string((int)p.y)
                    + " tick=" + std::to_string((long long)g_tick)
                    + " attempt=" + std::to_string(g_attempt));
            }
        }
        // When and with which rects a contact trigger such as a speed portal fired
        // (cfg `hitboxtrace=1`, `ptt:` line). Two cases where the firing tick was 1 tick
        // off from the model (lv18 t=14,613 / lv19 t=20,310) could not be fitted by a
        // single convention using the objrects boxes, so have GD dump both rects it uses
        // on the spot
        if (g_cfg.hitboxTrace && g_started && !g_sessionOver && object
            && player == m_player1) {
            static int lines = 0;
            if (++lines <= 2000) {
                const auto orct = object->getObjectRect();
                const auto prct = player->getObjectRect();
                char b[256];
                snprintf(b, sizeof(b),
                         "ptt: t=%lld uid=%d id=%d orect=(%.3f,%.3f,%.3f,%.3f) "
                         "prect=(%.3f,%.3f,%.3f,%.3f)",
                         (long long)g_tick, object->m_uniqueID,
                         object->m_objectID,
                         orct.origin.x, orct.origin.y,
                         orct.size.width, orct.size.height,
                         prct.origin.x, prct.origin.y,
                         prct.size.width, prct.size.height);
                writeResult(b);
            }
        }
        GJBaseGameLayer::playerTouchedTrigger(player, object);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        // Replay mode: block real input other than injections (still recorded).
        // !g_sessionOver is required: without it the block persists after the session
        // ends and manual play stops working
        if (g_cfg.blockInput && g_started && !g_sessionOver && !g_injecting) {
            ev("handleButton_BLOCKED", down, button, isPlayer1);
            return;
        }
        ev("handleButton", down, button, isPlayer1);
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    int checkCollisions(PlayerObject* player, float dt, bool ignoreDamage) {
        bool isP1 = (player == m_player1);
        // Do not skip checkCollisions wholesale here: GD's collision state would go stale,
        // and the first test right after releasing noclip misdetects and dies instantly.
        // Pass-through is done via the no-op on the collidedWithObject side
        if (isP1) ev("checkCollisions", dt);
        return GJBaseGameLayer::checkCollisions(player, dt, ignoreDamage);
    }
};
