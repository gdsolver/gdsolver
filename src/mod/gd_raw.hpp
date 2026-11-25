#pragma once
// Members the game has (they sit at these offsets of the 2.2091 executable) that Geode 6's bindings do not name.
// Reached by offset from the object, with the type the game uses. The values were measured on the 5-series build's own
// bindings for 2.2091 (the offsetof of the names that bindings carried); nothing here is checked by the compiler, so a
// new game build needs them re-read (lab: layout_scan.py).
#include <cstddef>
#include <map>
#include <unordered_set>

namespace gdraw {
template <class T>
inline T& at(void* obj, std::size_t off) { return *reinterpret_cast<T*>(reinterpret_cast<char*>(obj) + off); }

inline constexpr std::size_t kPlayerRingRelatedSet = 2128;   // std::unordered_set<int>
inline constexpr std::size_t kPlayerJumpPadRelated = 2976;   // std::map<int, bool>

template <class P> inline std::unordered_set<int>& ringRelatedSet(P* p) { return at<std::unordered_set<int>>(p, kPlayerRingRelatedSet); }
template <class P> inline std::map<int, bool>& jumpPadRelated(P* p) { return at<std::map<int, bool>>(p, kPlayerJumpPadRelated); }
}  // namespace gdraw
