#pragma once
// The anchor payload's `ever` / `ever2` (owns=ever; ever=<uid>,<uid>,...; ever2=<uid>,...): the objects whose EnhancedGameObject::m_activatedByPlayer1 / m_activatedByPlayer2 flag was set at the
// anchor's tick, as the mod read them (repair.hpp scanActivated). The flag is GD's once-per-attempt state for portals, pads, rings and orbs: it is set when the body first touches the object, it is
// not cleared until the attempt's reset (measured: 50 samples over 25,000 ticks of one flight, each set contains the one before), and the two bodies have their own. (The std::map at layer+0x4f0 that
// FUN_14031ef50 keeps is NOT this: it holds only the objects in contact at the moment and loses them when the contact ends -- 39 of 5,002 ticks non-empty, never more than 2 entries.)
// Without the payload the shadow seeds the once-memory by a rule (SlWorld::everRuleSays / slRuleSays: every static seedable object whose right edge lies behind the player was seen), which is a guess:
// a body that teleported in, or turns round after the anchor, reaches objects the rule calls seen that GD never touched (a custom level t=19076: a 4x speed portal 55 px behind the anchor, first
// touched by the ship after it turned; the pass fired it as a repeat and took no speed). With the payload an object behind the anchor is seen exactly when GD saw it, and the rule
// is not consulted at all: the objects the run covers after the anchor enter the memory when the pass overlaps them (Ctx::everNow), as in the game.
//   g_everPayloadSet  : player 1's uids (read-only once the search starts; every shadow thread reads it)
//   g_everPayloadSet2 : player 2's uids (the second body's pass takes this one as its Ctx::everBase, SelfShadow::second)
// reset.hpp puts them back; cli.hpp sets them from the anchor payload. Off (no payload): nothing here is consulted and every shadow output is as it was.
#include "dp/flat_set.hpp"

namespace dp {

inline bool g_ownsEver = false;
inline bool g_everPayloadOn = false;
inline FlatSet<int> g_everPayloadSet, g_everPayloadSet2;

}  // namespace dp
