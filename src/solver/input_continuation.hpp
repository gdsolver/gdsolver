#pragma once
#include <utility>

namespace p1::solver {

// A bot checkpoint must retain pending actions, not infer them from a held button.
template <class Player>
struct InputContinuation {
    decltype(std::declval<Player>().m_holdingButtons) holdingButtons;
    bool present = false, jumpBuffered = false, wasJumpBuffered = false;
    bool stateRingJump = false, touchedPad = false;
    unsigned char stateJumpBuffered = 0;
};

// Copy the container by value; later releases must not mutate the saved input.
template <class Player>
void captureInputContinuation(Player* p, InputContinuation<Player>& out) {
    if (!p) { out = {}; return; }
    out.present = true;
    out.holdingButtons = p->m_holdingButtons;
    out.jumpBuffered = p->m_jumpBuffered;
    out.wasJumpBuffered = p->m_wasJumpBuffered;
    out.stateRingJump = p->m_stateRingJump;
    out.stateJumpBuffered = p->m_stateJumpBuffered;
    out.touchedPad = p->m_touchedPad;
}

// Restore without push/release callbacks, which can fire rings or stop a dash.
template <class Player>
void restoreInputContinuation(Player* p, const InputContinuation<Player>& in) {
    if (!p || !in.present) return;
    p->m_holdingButtons = in.holdingButtons;
    p->m_jumpBuffered = in.jumpBuffered;
    p->m_wasJumpBuffered = in.wasJumpBuffered;
    p->m_stateRingJump = in.stateRingJump;
    p->m_stateJumpBuffered = in.stateJumpBuffered;
    p->m_touchedPad = in.touchedPad;
}

} // namespace p1::solver
