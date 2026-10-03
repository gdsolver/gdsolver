#pragma once
#include <utility>

namespace p1::solver {

// Delayed player-follow reads a circular height history, not just the player's current Y.
template <class Player>
struct PlayerFollowState {
    int cursor = 0;
    decltype(std::declval<Player>().m_playerFollowFloats) heights;
};

// Copy the history together with its cursor; both are saved by PlayerCheckpoint.
template <class Player>
inline void capturePlayerFollow(Player* player, PlayerFollowState<Player>& out) {
    out.cursor = player->m_followRelated;
    out.heights = player->m_playerFollowFloats;
}

// Preserve the live vector owner while replacing another branch's history, including empties.
template <class Player>
inline void restorePlayerFollow(Player* player, const PlayerFollowState<Player>& in) {
    player->m_followRelated = in.cursor;
    player->m_playerFollowFloats = in.heights;
}

} // namespace p1::solver
