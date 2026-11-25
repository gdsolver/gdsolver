#pragma once
// Members the game has that the code reaches by offset from the object, with the type the game uses.
// Geode 6's bindings do name both of these (m_usedRings, m_touchedForceIds), so the offset is the compiler's own on every
// platform: 2128 / 2976 on Windows (the values this header held as literals), 0x818 / 0xb38 on android64 2.2091. The accessors
// stay so that the sites read as they did.
#include <Geode/Geode.hpp>

#include <cstddef>
#include <map>
#include <unordered_set>

namespace gdraw {
template <class T>
inline T& at(void* obj, std::size_t off) { return *reinterpret_cast<T*>(reinterpret_cast<char*>(obj) + off); }

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#endif
inline constexpr std::size_t kPlayerRingRelatedSet = offsetof(PlayerObject, m_usedRings);        // std::unordered_set<int>
inline constexpr std::size_t kPlayerJumpPadRelated = offsetof(PlayerObject, m_touchedForceIds);  // std::map<int, bool>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#if defined(GEODE_IS_WINDOWS) && GEODE_COMP_GD_VERSION >= 22091
static_assert(kPlayerRingRelatedSet == 2128 && kPlayerJumpPadRelated == 2976, "the values this header held before the bindings named them");
#endif

template <class P> inline std::unordered_set<int>& ringRelatedSet(P* p) { return at<std::unordered_set<int>>(p, kPlayerRingRelatedSet); }
template <class P> inline std::map<int, bool>& jumpPadRelated(P* p) { return at<std::map<int, bool>>(p, kPlayerJumpPadRelated); }
}  // namespace gdraw