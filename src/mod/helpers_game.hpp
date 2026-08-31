#pragma once
// Audio control and fast-mode predicates shared by the hooks.
#include "mod/repair.hpp"

using namespace p1;

// Whether fast mode is actually in effect (accounts for the F8 realtime override)
inline bool fastModeActive() {
    return g_cfg.fastdt > 0 && !g_realtimeOverride;
}

// Declared in session_hotkeys.hpp (namespace p1), where probe:: is not in scope yet
namespace p1 {
inline void clearManualPause() {
    probe::g_pause = false;
    probe::g_step = 0;
}
// Whether the manual (F2) stop is on. Declared next to clearManualPause in session_hotkeys.hpp
// because the keys have to ask -- while the game is stopped the arrows are step keys, not seek
// keys, and probe:: is not visible that early in the include chain.
inline bool manualPaused() { return probe::g_pause; }
// ...and whether a frame-step is still waiting to be executed. A backward step finishes with one
// of these (it aims short and walks the last ticks exactly), and asking for another step before
// that has run compounds two half-finished moves.
inline bool manualStepPending() { return probe::g_step > 0; }
}  // namespace p1

// ---- Audio ----
// Only the audio API is touched here. Game state, physics and the tick stream are not, so
// nothing in this namespace can change a run.
//
// Two jobs:
//  1. SILENCE WHILE SOLVING [2026-08-23, user decision]. A solve is hundreds of attempts a
//     minute and the physics runs tens of times faster than the song, so the audio is noise
//     at best. cfg `music=mute` silences a session the same way (that is what the workers
//     pass).
//  2. KEEP THE SONG WITH THE REPLAY. Stopping the game stops the song, and the spectating
//     notch retunes it, so what is heard matches what is seen. FMOD's pitch IS its playback
//     rate, so following the notch costs nothing and needs no extra library -- but it shifts
//     the pitch with the speed, and past kPitchMax that is not worth listening to. There the
//     song is paused instead and the needle is put back when the speed drops again.
namespace audio {

// Above this notch the song is paused rather than played (a 8x/16x chipmunk is not music)
constexpr float kPitchMax = 4.f;

inline bool  g_silenced = false;     // the "solving / music=mute" stop has been issued
inline bool  g_musicPaused = false;  // WE paused it (never fight a pause GD itself made)
inline bool  g_pausedForSpeed = false; // ...and it was the speed, not a game pause, that did it
inline float g_pitch = 1.f;          // the pitch currently applied to the music group
// Whether the group has had to be told again since the last real transition. GD taking the song
// back is worth one line; re-asserting it every frame afterwards is worth none.
inline bool  g_reasserted = false;

// A session that is driving the game and must not make a sound.
//
// SILENCE FOLLOWS THE SCREEN, not the session. A solve runs dark and fast by default -- hundreds
// of attempts a minute with the physics tens of times faster than the song -- and there sound is
// noise at best; the song restarting the moment the first candidate died was the most jarring
// thing about the panel's Solve mode. But the screen is one keypress away (F5), and a run being
// watched at normal speed is a replay whatever the session is called, so the sound comes back
// with the picture. Anything else means pressing F5 mid-solve gives you a silent film.
//
// The pausing and the pitch are audio::sync's job below. This only decides whether there is
// anything to hear at all -- which is why the frozen level during a search needs no case here:
// sync pauses the song with the game.
// Sound EFFECTS only, and only while the screen is blacked out for a seek. Not silent() below:
// that one stops the music outright, and a seek that stopped the song would arrive at a silent
// level (nothing restarts it mid-attempt). The song is handled properly in sync() -- paused for
// the duration and re-seeked to the tick the game arrived at -- so all that is left to suppress
// is the hundreds of orb, portal and death effects a fast-forward fires behind the cover.
inline bool effectsMuted() { return itermap::seeking(); }

inline bool silent() {
    if (g_cfg.music == "mute") return true;      // what the driver's workers pass
    // The SECTION search, before anything else -- including the showing flag and the screen.
    // It is the one phase that is not a run at all: tens of thousands of restores and swallowed
    // deaths, none of them on the timeline the song belongs to. Whether anybody is looking does
    // not make that audible (the note on g_secSearching in config.hpp).
    if (g_secSearching) return true;
    if (g_dpShowSolution) return false;          // the showing is a replay in every sense
    if (g_cfg.dpSolve) return !renderingOn() || fastModeActive();
    return solvingNow();
}

// Where the song should be, taken from the physics clock: the substep is 1/240s and the song
// starts with the attempt, so the tick counter is the song position. Only used for the seek
// after the song was paused while the game ran on
inline unsigned songPosMs() { return (unsigned)((double)g_tick * (1000.0 / 240.0)); }

inline FMOD::ChannelGroup* musicGroup() {
    auto* fe = FMODAudioEngine::sharedEngine();
    return fe ? fe->m_backgroundMusicChannel : nullptr;
}

// ---- what the group is ACTUALLY holding ----
//
// As opposed to what we last asked it for, which is what sync() used to latch on -- and latching
// on our own last request is how the pause and the pitch both went missing.
//
// GD restarts the song whenever it likes, and it does not know we had an opinion about it. The
// render key's return path is the sharpest case: resuming rendering after a blind stretch runs
// resetLevel() to rebuild the visibility state (hooks_gamelayer.cpp), GD starts the song again
// as part of that, and the new song arrives unpaused at pitch 1. The latch then said "already
// done" and nothing touched it again for the rest of the session.
//
// Reported 2026-08-31: Solve, raise the notch with the arrows, F5 to bring the screen back --
// and the song plays on at 1x. The log has the whole bug in two lines: `hotkey: F5 -> render ON`
// followed by `audio: music paused (the game is stopped)`, with the music audible anyway.
//
// The seek branch in sync() already learned this ("FORCED EVERY FRAME rather than latched on a
// transition") and for the same reason. This is that fix with the latch kept honest: ask, and
// write only when it really differs -- so the transition logic, which SEEKS the song on the way
// back, still runs exactly once on the real transition and not every frame.
//
// The out-parameter is seeded with something that is NOT the wanted value, so a query that
// fails cannot read as "already right".
inline bool pausedIs(FMOD::ChannelGroup* grp, bool want) {
    bool cur = !want;
    grp->getPaused(&cur);
    return cur == want;
}
inline bool pitchIs(FMOD::ChannelGroup* grp, float want) {
    float cur = want + 1.f;
    grp->getPitch(&cur);
    return cur == want;
}

// ---- the seek's mute ----
//
// Pausing the group is not enough on its own. A backward seek RESTARTS the level, GD starts the
// song from the top as part of that reset, and sync() -- which runs once a frame -- does not get
// to pause it until the frame after. That gap is audible: the first fraction of a second of the
// track plays on every rewind.
//
// So the volume is taken to zero at the moment the restart is DECIDED, before resetLevel runs,
// and put back when the seek ends. Volume rather than a stop, for the same reason silent() is
// not used here: a stopped song does not come back mid-attempt, and the seek would arrive at a
// silent level.
inline float g_savedVolume = -1.f;   // <0 = we have not taken it

// ---- silence by VOLUME ----
//
// Stopping the individual sounds does not hold, and three rounds of trying taught the same thing
// each time: act once and something acts after you. A stop issued at the moment silence begins
// cannot reach what starts afterwards; hooking the entry points only reaches the doors you know
// about (a whole lv21 solve made a sound while ONE call arrived at the four that were hooked);
// and stopping again on a count only reached the music, so the next run leaked effects instead.
//
// The volume is the one place all of it has to pass. Forced to zero every frame for as long as
// the mod should be quiet, and put back when it should not, so nothing that starts in between has
// anywhere to be audible. This is the engine's live level, not the saved setting, and it is
// restored on the way out of silence and again in neutral() -- but the sentinels matter: capture
// only when we have not already taken it, or a second capture stores the zero we just wrote and
// the user's real level is gone.
inline float g_savedMusicVol = -1.f;
inline float g_savedSfxVol = -1.f;

inline void hushVolumes() {
    auto* fe = FMODAudioEngine::sharedEngine();
    if (!fe) return;
    if (g_savedMusicVol < 0.f) g_savedMusicVol = fe->getBackgroundMusicVolume();
    if (g_savedSfxVol < 0.f) g_savedSfxVol = fe->getEffectsVolume();
    fe->setBackgroundMusicVolume(0.f);
    fe->setEffectsVolume(0.f);
}

inline void restoreVolumes() {
    if (g_savedMusicVol < 0.f && g_savedSfxVol < 0.f) return;
    if (auto* fe = FMODAudioEngine::sharedEngine()) {
        if (g_savedMusicVol >= 0.f) fe->setBackgroundMusicVolume(g_savedMusicVol);
        if (g_savedSfxVol >= 0.f) fe->setEffectsVolume(g_savedSfxVol);
    }
    g_savedMusicVol = -1.f;
    g_savedSfxVol = -1.f;
}

// ---- the song a seek was not allowed to start ----
//
// Muting the channel group before resetLevel does NOT work, and this is why: the reset makes GD
// start the track again, on a channel it creates as part of that, so the volume was taken to zero
// on a group that is then replaced. Pausing it does not work either -- audio::sync runs earlier in
// the frame than the restart does, so the pause lands a whole frame late, and a frame of the
// track's opening is exactly what you hear.
//
// So the call itself is held. While a seek is running, playMusic is not passed through; its
// arguments are kept, and the arrival plays it and seeks it to where the game ended up. Nothing
// is lost -- the song that would have started is the song that starts, one seek later and in the
// right place.
inline bool g_musicHeld = false;
inline gd::string g_heldPath;
inline bool g_heldLoop = false;
inline float g_heldFade = 0.f;
inline int g_heldChannel = 0;

inline void holdMusic(const gd::string& path, bool loop, float fade, int channel) {
    g_musicHeld = true;
    g_heldPath = path;
    g_heldLoop = loop;
    g_heldFade = fade;
    g_heldChannel = channel;
}

inline void muteForSeek() {
    auto* grp = musicGroup();
    if (!grp) return;
    if (g_savedVolume < 0.f) {
        float v = 1.f;
        grp->getVolume(&v);
        g_savedVolume = v;
    }
    grp->setVolume(0.f);
}

inline void unmuteAfterSeek() {
    if (g_savedVolume < 0.f) return;
    if (auto* grp = musicGroup()) grp->setVolume(g_savedVolume);
    g_savedVolume = -1.f;
}

// Silence the song RIGHT NOW, on whatever channel group exists at this instant. Called straight
// after resetLevel, because that is the one moment sync() cannot cover: sync runs earlier in the
// frame than the restart does, so the fresh song GD starts as part of the reset gets a whole
// frame at full volume before anything else looks at it -- and one frame of the track's opening
// is precisely what a rewind sounded like.
inline void hushNow() {
    auto* grp = musicGroup();
    if (!grp) return;
    if (g_savedVolume < 0.f) {
        float v = 1.f;
        grp->getVolume(&v);
        g_savedVolume = v;
    }
    grp->setVolume(0.f);
    grp->setPaused(true);
    g_musicPaused = true;
    g_pausedForSpeed = true;    // the arrival must put it back where the game ended up
}

// Put the engine back the way GD expects to find it. Called when no session is driving and
// on the way out of a level -- a leftover pitch would follow the player into the menu.
inline void neutral() {
    unmuteAfterSeek();   // ...and a leftover mute would follow them in silence
    restoreVolumes();    // ...as would a volume the mod took to zero and never gave back
    // A song held for a seek that never landed (the session ended first) belongs to the level
    // that is being left. Dropping it here stops it being played into the next one.
    g_musicHeld = false;
    if (auto* grp = musicGroup()) {
        if (g_pitch != 1.f) grp->setPitch(1.f);
        if (g_musicPaused) grp->setPaused(false);
    }
    g_pitch = 1.f;
    g_musicPaused = false;
    g_pausedForSpeed = false;
    g_silenced = false;
}

// One pass, called once per frame from the game layer's update. Everything is latched: the
// audio API is only touched when the state actually changes.
inline void sync() {
    auto* fe = FMODAudioEngine::sharedEngine();
    if (!fe) return;
    if (!g_started || g_sessionOver) { neutral(); return; }
    if (silent()) {
        if (!g_silenced) {
            g_silenced = true;
            g_musicPaused = false;
            g_pitch = 1.f;
            fe->stopAllMusic(false);
            fe->stopAllEffects();
            log::info("audio: silent ({})", solvingNow() ? "solving"
                                          : (g_cfg.dpSolve ? "solve session" : "music=mute"));
        }
        // ...and the volume held at zero for as long as this lasts. See hushVolumes: the stop
        // above is a one-shot and cannot reach anything that starts after it, which is what kept
        // leaking -- first the music, then, once the music was stopped again, the effects.
        hushVolumes();
        ++g_silenceReasserts;
        return;
    }
    restoreVolumes();   // out of silence: give the levels back before anything else runs
    g_silenced = false;
    // The seek's mute is TAKEN here and RELEASED at the very end of this function, after the song
    // has been put back where the game arrived. Releasing it up here played the track from its
    // beginning on every rewind: a rewind restarts the level, GD starts the song at zero, and the
    // resume below unpauses before it seeks -- so with the volume already back, the first moment
    // of the track got out between the two.
    if (itermap::seeking()) muteForSeek();
    auto* grp = fe->m_backgroundMusicChannel;
    if (!grp) return;
    // The game is stopped (F2, or the driver's pause) -> the song stops with it. Above the
    // pitch ceiling it stops too, but for the opposite reason: there the game runs on
    const bool gameStopped = g_paused || probe::g_pause;
    // A SEEK is the same case as the pitch ceiling and is handled by the same branch: the level is
    // running at hundreds of batches a frame while the song plays at one, so leaving it alone
    // leaves the two hundreds of seconds apart by the time the seek lands. Pausing it here means
    // the resume below re-seeks the song to the tick the game actually arrived at, which is the
    // whole reason g_pausedForSpeed exists.
    // WHILE A SEEK RUNS, FORCED EVERY FRAME rather than latched on a transition.
    //
    // Two attempts failed before this one, and both failed the same way: they acted once and
    // something else acted after them. Zeroing the volume before resetLevel zeroed a group the
    // reset then replaced; pausing through the latch below never fired twice, so whatever the
    // reset did to the song afterwards simply stood. The song is GD's to restart at any point in
    // its own reset, so the only thing that holds is re-asserting it on the group as it is NOW,
    // every frame, for as long as the seek lasts.
    if (itermap::seeking()) {
        muteForSeek();              // re-fetches the group, so a replaced one is caught
        grp->setPaused(true);
        if (!g_musicPaused) {
            g_musicPaused = true;
            g_pausedForSpeed = !gameStopped;   // the arrival must re-seek it
            log::info("audio: music held for the seek");
        }
        return;
    }
    const bool tooFast = g_watchSpeed > kPitchMax;
    const bool wantPaused = gameStopped || tooFast;
    if (wantPaused != g_musicPaused) {
        g_musicPaused = wantPaused;
        g_reasserted = false;
        if (wantPaused) {
            grp->setPaused(true);
            // Remember WHY. Only the speed stop lets the game run on without the song, and
            // only that one needs a seek on the way back; a game pause stopped both together
            // and resumes in step
            g_pausedForSpeed = !gameStopped;
            log::info("audio: music paused ({})",
                      gameStopped ? "the game is stopped"
                                  : (itermap::seeking() ? "seeking"
                                                        : "above the pitch ceiling"));
        } else if (g_pausedForSpeed) {
            // SEEK FIRST, THEN UNPAUSE. The other order lets the track play from wherever it
            // was parked -- position zero, after a rewind restarted the level -- for however
            // long FMOD takes to act on the seek.
            g_pausedForSpeed = false;
            fe->setMusicTimeMS(songPosMs(), false, 0);
            grp->setPaused(false);
            log::info("audio: music resumed, seeked to {}ms (tick {})",
                      songPosMs(), (long long)g_tick);
        } else {
            grp->setPaused(false);
            log::info("audio: music resumed");
        }
    } else if (wantPaused && !pausedIs(grp, true)) {
        // The song was replaced under a pause this had already latched. Put it back on the group
        // as it is NOW -- the note at pausedIs has the case that produced it.
        grp->setPaused(true);
        if (!g_reasserted) {
            g_reasserted = true;
            log::info("audio: the song came back unpaused (GD restarted it) - pause re-asserted");
        }
    }
    // ...and the pitch, checked against the group rather than against our own record of what we
    // asked for.
    if (!wantPaused && !pitchIs(grp, g_watchSpeed)) {
        if (g_watchSpeed != g_pitch) {
            g_reasserted = false;     // a new intent, not a re-assert
            log::info("audio: pitch {:g}x (follows the spectating speed)", (double)g_watchSpeed);
        } else if (!g_reasserted) {
            g_reasserted = true;
            log::info("audio: the song came back at pitch 1 (GD restarted it) - {:g}x re-applied",
                      (double)g_watchSpeed);
        }
        g_pitch = g_watchSpeed;
        grp->setPitch(g_watchSpeed);
    }
    // The song the seek held back (see holdMusic). Started here, at the end, once the game has
    // stopped moving -- and seeked straight to where it stopped, so it comes in on the beat
    // rather than from the top of the track.
    if (!itermap::seeking() && g_musicHeld) {
        g_musicHeld = false;
        fe->playMusic(g_heldPath, g_heldLoop, g_heldFade, g_heldChannel);
        fe->setMusicTimeMS(songPosMs(), false, 0);
        g_musicPaused = false;
        g_pausedForSpeed = false;
        log::info("audio: the held song started at {}ms (tick {})",
                  songPosMs(), (long long)g_tick);
    }
    // LAST. Everything above has put the song where the game is; only now is it safe to be
    // audible again (see muteForSeek).
    if (!itermap::seeking()) unmuteAfterSeek();
}

}  // namespace audio
