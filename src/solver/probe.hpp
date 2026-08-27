#pragma once
// Manual observation controls: freeze the game and step it by hand.
//
// This used to also hold the "cheat" side -- a one-key warp that carried the player through
// geometry, a persistent noclip, forcing a mode, and an infinite jump. Those were removed on
// 2026-08-23: they let a human do things the game does not allow, which is not what this
// project is for, and every one of them was a way to reach a state that no real run can.
// What is left is the part that only stops time: it changes nothing about the physics, it just
// lets a person look at it.
namespace probe {

// Pause/step for manual experiments. The session-wide g_paused only works while a session is
// open (g_started), so this is a separate one that also works during plain play.
inline bool g_pause = false;
inline int g_step = 0;   // number of substeps to advance on the next frame

inline void reset() {
    g_pause = false;
    g_step = 0;
}

// Log the state as one line (for pinpointing causes)
inline void trace(const char* tag, PlayerObject* p, long long tick) {
    if (!p) return;
    writeResult(std::string("PROBE[") + tag + "] tick=" + std::to_string(tick)
        + " x=" + std::to_string(p->getPositionX())
        + " y=" + std::to_string(p->getPositionY())
        + " vy=" + std::to_string((float)p->m_yVelocity)
        + " g=" + std::to_string(p->m_isOnGround)
        + " g2=" + std::to_string(p->m_isOnGround2)
        + " dead=" + std::to_string(p->m_isDead)
        + " up=" + std::to_string(p->m_isUpsideDown)
        + " locked=" + std::to_string(p->m_isLocked)
        + " ctrlOff=" + std::to_string(p->m_controlsDisabled)
        // Emit both the node coordinates and GD's physics coordinates (m_position)
        // (the symptom of two different writers alternating is told apart by
        // the mismatch between these two)
        + " mpos=" + std::to_string(p->m_position.y)
        + " gnd=" + std::to_string(
            PlayLayer::get() && PlayLayer::get()->m_groundLayer
                ? PlayLayer::get()->m_groundLayer->getPositionY() : -1.f)
        + " mode=" + modeStr(p));
}

} // namespace probe
