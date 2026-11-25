// Compile-time check of src/mod/gd_offsets.hpp: every raw offset must be where the bindings put
// the member it stands for, on the platform being built. Nothing here runs.
//
// On Windows this holds the literals the code has always used against the bindings' layout. On
// android64 it holds the values measured on the Android binary against the same bindings as
// the Android compiler lays them out, which is the check that the layout the measurement
// assumed is the one the mod is built with. The RVAs of the three seeds are not members and
// have no check here; tools/android_offsets.py finds them in the binary.
#include <Geode/Geode.hpp>

#include <cstddef>

#include "mod/gd_offsets.hpp"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#endif

namespace {
using namespace gdoff;
using cocos2d::CCPoint;

constexpr std::size_t kGameState = offsetof(GJBaseGameLayer, m_gameState);
#define STATE(m) (kGameState + offsetof(GJGameState, m))

// GJBaseGameLayer
// (m_unkPoint17 is not named by the Geode 6 bindings: kLayerUnkPoint17Y stays a measured offset, no check)
// static_assert(kLayerUnkPoint17Y == STATE(m_unkPoint17) + offsetof(CCPoint, y));
static_assert(kLayerCollisionBlockSectionSizes
              == offsetof(GJBaseGameLayer, m_collisionBlockSectionSizes));

// PlayerObject
static_assert(kPlayerTouchedRing + 1 == offsetof(PlayerObject, m_touchedCustomRing));
static_assert(kPlayerTouchedRing + 2 == offsetof(PlayerObject, m_touchedGravityPortal));

// GameObject and subclasses



// Menu layers

#undef STATE
// Sizes. A class the mod allocates through an inline create() is allocated at sizeof(), and the game's constructor then writes every byte the game has.
#if GEODE_COMP_GD_VERSION >= 22090 && defined(GEODE_IS_WINDOWS)
// 2.209's GJGameLevel is 0x840 bytes. Bindings that stop short of it make the first level the mod creates itself (the slice's copy) overflow the heap.
static_assert(sizeof(GJGameLevel) == 0x840, "the bindings' GJGameLevel must be the game's 0x840 bytes: GJGameLevel::create() allocates it inline");
// The bases the mod derives from (a derived class puts its own members at sizeof(base), and the base's constructor, in the game's or cocos' code, writes
// the real size) and the classes it creates itself. Each is the size the class's own create() allocates in 2.209 (libcocos2d.dll for cocos2d, the exe for
// the rest); CCObject's is the highest field its constructor writes.
static_assert(sizeof(cocos2d::CCObject) == 0x38);
static_assert(sizeof(cocos2d::CCNode) == 0x140);
static_assert(sizeof(cocos2d::CCNodeRGBA) == 0x158);
static_assert(sizeof(cocos2d::CCLayer) == 0x198);
static_assert(sizeof(cocos2d::CCLayerColor) == 0x220);
static_assert(sizeof(cocos2d::CCMenuItem) == 0x178);
static_assert(sizeof(FLAlertLayer) == 0x298);
static_assert(sizeof(CCMenuItemSpriteExtra) == 0x208);
static_assert(sizeof(ButtonSprite) == 0x2c8);
static_assert(sizeof(GJGameState) == 0x6d0);
// A struct the mod allocates and the game fills (psnap's EMSnap::full): GJEffectManager::saveToState reaches +0x338 (4 bytes) in 2.209, nothing there in 2.2081.
static_assert(sizeof(EffectManagerState) == 0x340);
// The element psnap keeps a vector of and assigns to and from the game's own vector (updatePulseEffects divides by 72 in both versions).
static_assert(sizeof(PulseEffectAction) == 72);
// The objects psnap and cpflight copy as raw bytes: the size the game frees (the scalar-deleting destructor in slot 1 of its vtable) is the size the copy must cover.
static_assert(sizeof(PlayerObject) == 0xc50);
static_assert(sizeof(GameObject) == 0x550);
static_assert(sizeof(EffectGameObject) == 0x740);
static_assert(sizeof(GJBaseGameLayer) == GD209(0x3978, 0x3938));   // the update took 0x40 bytes out at m_effectManager
static_assert(sizeof(PlayLayer) == GD209(0x3d20, 0x3ce0));
static_assert(sizeof(GJEffectManager) == 0x828);
#endif

}  // namespace

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
