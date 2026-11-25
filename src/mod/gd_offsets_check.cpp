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
static_assert(kLayerCollisionBlockSectionSizes
              == offsetof(GJBaseGameLayer, m_collisionBlockSectionSizes));
#if GEODE_COMP_GD_VERSION >= 22091
// kLayerUnkPoint17Y is the y of the point the Geode 6 bindings call m_staticCameraActive (getMinPortalY / getMaxPortalY read it as a float).
static_assert(kLayerUnkPoint17Y == STATE(m_staticCameraActive) + offsetof(CCPoint, y));
// The once-per-contact gate's map (gd_offsets.hpp kLayerGateMap) and the two section floats: Windows and android64 both, each held to the bindings.
static_assert(kLayerGateMap == STATE(m_activatedObjectIDs));
static_assert(kLayerSectionSizeX == offsetof(GJBaseGameLayer, m_sectionXFactor));
static_assert(kLayerSectionSizeY == offsetof(GJBaseGameLayer, m_sectionYFactor));
static_assert(kLayerLevelEndAnimStarted == offsetof(GJBaseGameLayer, m_levelEndAnimationStarted));
#endif

// PlayerObject
static_assert(kPlayerTouchedRing + 1 == offsetof(PlayerObject, m_touchedCustomRing));
static_assert(kPlayerTouchedRing + 2 == offsetof(PlayerObject, m_touchedGravityPortal));
#if GEODE_COMP_GD_VERSION >= 22091
static_assert(kPlayerBallStake == offsetof(PlayerObject, m_isRotating));
#endif

// GameObject and subclasses



// Menu layers

#undef STATE

#if defined(GEODE_IS_WINDOWS) && GEODE_COMP_GD_VERSION >= 22091
// The Windows values of the members that replaced a literal in the code (the literal is on the right): nothing may move on Windows.
static_assert(kStateMaxPortalY == 0x5bc && kPlayerDashY == 0x578 && kPlayerSlopeVelocity == 0x9bc && kPlayerWasOnSlope == 0x9b9);
static_assert(kPlayerIsShip == 0x9c1 && kPlayerIsBird == 0x9c2 && kPlayerIsBall == 0x9c3 && kPlayerIsDart == 0x9c4 && kPlayerIsSwing == 0x9cc);
static_assert(kPlayerIconSprite == 0x740 && kPlayerGameLayer == 0xc28 && kPlayerCanRunIntoBlocks == 0x7ed);
static_assert(kPlayerDashFireSprite == 0x668 && kPlayerIsBallRotating == 0x670 && kPlayerCurrentPotentialSlope == 0x678 && kPlayerNoPotentialSlope == 0x671);
static_assert(kObjMiddleSectionIndex == 0x27c && kObjOuterSectionIndex == 0x278 && kObjSomeOtherIndex == 0x270 && kObjIsActivated == 0x305);
static_assert(kObjGroupDisabled == 0x28e && kObjPassable == 0x515 && kObjLastPosition == 0x4d0 && kObjLastFrameStamp == 0x4dc);
static_assert(kPlayerStateHitHead == 0xb84 && kPlayerStateFlipGravity == 0xb88 && kPlayerUpsideDown == 0x9c7 && kPlayerPlatformer == 0xb78);
static_assert(kPlayerCurrentSlope == 0x680 && kEffSpecialTarget == 0x614);
#endif// Sizes. A class the mod allocates through an inline create() is allocated at sizeof(), and the game's constructor then writes every byte the game has.
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
#if GEODE_COMP_GD_VERSION >= 22091 && defined(GEODE_IS_ANDROID64)
// The same sizes on android64 (2.2091), each the number the game's own create() hands operator new (libcocos2dcpp.so: GJGameLevel::create 0x80c678,
// PlayerObject::create 0x76825c, EffectGameObject::create 0x876804, PlayLayer::create 0x712900, GJEffectManager::create 0x9f970c).
static_assert(sizeof(GJGameLevel) == 0x740, "the bindings' GJGameLevel must be the game's 0x740 bytes: GJGameLevel::create() allocates it inline");
static_assert(sizeof(PlayerObject) == 0xbf0);
static_assert(sizeof(EffectGameObject) == 0x730);
static_assert(sizeof(PlayLayer) == 0x3a38);
static_assert(sizeof(GJEffectManager) == 0x678);
#endif

}  // namespace

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
