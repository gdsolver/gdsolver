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

// Put the engine back the way GD expects to find it. Called when no session is driving and
// on the way out of a level -- a leftover pitch would follow the player into the menu.
inline void neutral() {
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
            log::info("audio: silent ({})", solvingNow() ? "solving"
                                          : (g_cfg.dpSolve ? "solve session" : "music=mute"));
        }
        return;
    }
    g_silenced = false;
    auto* grp = fe->m_backgroundMusicChannel;
    if (!grp) return;
    // The game is stopped (F2, or the driver's pause) -> the song stops with it. Above the
    // pitch ceiling it stops too, but for the opposite reason: there the game runs on
    const bool gameStopped = g_paused || probe::g_pause;
    const bool tooFast = g_watchSpeed > kPitchMax;
    const bool wantPaused = gameStopped || tooFast;
    if (wantPaused != g_musicPaused) {
        g_musicPaused = wantPaused;
        grp->setPaused(wantPaused);
        if (wantPaused) {
            // Remember WHY. Only the speed stop lets the game run on without the song, and
            // only that one needs a seek on the way back; a game pause stopped both together
            // and resumes in step
            g_pausedForSpeed = !gameStopped;
            log::info("audio: music paused ({})",
                      gameStopped ? "the game is stopped" : "above the pitch ceiling");
        } else if (g_pausedForSpeed) {
            g_pausedForSpeed = false;
            fe->setMusicTimeMS(songPosMs(), false, 0);
            log::info("audio: music resumed, seeked to {}ms (tick {})",
                      songPosMs(), (long long)g_tick);
        } else {
            log::info("audio: music resumed");
        }
    }
    if (!wantPaused && g_watchSpeed != g_pitch) {
        g_pitch = g_watchSpeed;
        grp->setPitch(g_watchSpeed);
        log::info("audio: pitch {:g}x (follows the spectating speed)", (double)g_watchSpeed);
    }
}

}  // namespace audio
