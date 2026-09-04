#pragma once
namespace orbtrace {
inline long long g_lastPress = -1; // most recent press tick
inline int g_lines = 0;            // total log lines (cap against flooding)
inline void reset() { g_lastPress = -1; }
}
namespace padtrace {
inline int g_lines = 0;            // total log lines (cap against flooding)
inline void reset() { g_lines = 0; }
}
// What the anchor payload is built from: the tick GD activated each touch
// trigger. dp's --start carries the physics state and not the values whose
// worth depends on earlier ticks, so a re-anchored solve begins with every
// door shut; reading it back out of the moving-geometry recording works only
// for objects the recording contains, which left 163 differences on lv22.
//
// The activation itself is observable, so nothing has to be inferred:
// EnhancedGameObject::activatedByPlayer is the only setter of the flag the
// spawn queue reads (+0x5b3), it is cleared only by resetObject, and triggers
// reach it because EffectGameObject derives from EnhancedGameObject.
//
// FIRST activation, and p1's only. First, because the flag latches within an
// attempt and dp writes its own fire tick once (markTouched skips a box whose
// bit is set). p1's only, because dp's markTouched reads p1's position -- a
// bit set from p2 would be one the model can never set going forward, so an
// anchor would claim what a whole run of the same plan would not.
namespace touchseed {
inline std::unordered_map<int, int> g_first;   // trigger uid -> first tick
// Activations by the SECOND player, counted and never carried. dp cannot
// express them, and the model-side check cannot even look: it compares against
// the model's own whole run, and lv20 -- the only level with both a dual
// portal and touch boxes -- dies at t=1,157 while its dual section starts near
// t=17,119. This counter runs inside GD, which replays lv20 to the end, so it
// is the one observer that reaches the case.
inline int g_p2 = 0;
inline void reset() { g_first.clear(); g_p2 = 0; }
}
