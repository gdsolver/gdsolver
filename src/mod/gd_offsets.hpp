#pragma once
// ============================================================
// Raw offsets into GD's objects and data, per platform.
//
// Every offset the mod reads without a bindings name lives here, as GDOFF(win, android64).
// The Windows value is the 2.2081 literal the code used before this header existed, unchanged,
// so the Windows build compiles to the same instructions. The android64 value was measured on
// libcocos2dcpp.so, arm64-v8a, 2.2081, sha256
//   dda3752ab3a912fd2293561e157ecc98eca799056abd9223a331c1c514175df7
// by finding each field in the Android function the Windows comment names (the Windows
// function and address are given where the value is used). tools/android_offsets.py re-checks
// every value against the bindings and that binary.
//
// The member each offset belongs to is named beside it. src/mod/gd_offsets_check.cpp holds
// each value against offsetof(<class>, <member>) on both platforms, so a bindings update or
// a wrong measurement fails the build rather than reading the neighbouring field.
//
// A new raw offset goes here, with both values. A platform without values stops the build.
// ============================================================
#include <Geode/platform/cplatform.h>

#include <cstddef>
#include <cstdint>

#if defined(GEODE_IS_WINDOWS)
#define GDOFF(win, android64) (win)
#elif defined(GEODE_IS_ANDROID64)
#define GDOFF(win, android64) (android64)
#else
#error "gd_offsets.hpp: no raw offsets for this platform (Windows and android64 only)"
#endif

namespace gdoff {

// ---- GJBaseGameLayer (m_gameState is embedded at +0x1a8 on both) ----
inline constexpr std::size_t kLayerCameraZoom = GDOFF(0x1a8, 0x1a8);        // m_gameState.m_cameraZoom
inline constexpr std::size_t kLayerCameraOffsetY = GDOFF(0x1b4, 0x1b4);     // m_gameState.m_cameraOffset.y
inline constexpr std::size_t kLayerUnkPoint17Y = GDOFF(0x23c, 0x23c);       // m_gameState.m_unkPoint17.y
inline constexpr std::size_t kLayerUnkBool1 = GDOFF(0x2a0, 0x2a0);          // m_gameState.m_unkBool1
inline constexpr std::size_t kLayerFreeMode = GDOFF(0x311, 0x311);          // m_gameState.m_isFreeMode
inline constexpr std::size_t kLayerCurrentChannel = GDOFF(0x33c, 0x33c);    // m_gameState.m_currentChannel
inline constexpr std::size_t kLayerSpawnChannel0 = GDOFF(0x348, 0x348);     // m_gameState.m_spawnChannelRelated0
inline constexpr std::size_t kLayerSpawnChannel1 = GDOFF(0x388, 0x380);     // m_gameState.m_spawnChannelRelated1
inline constexpr std::size_t kLayerUnkBool10 = GDOFF(0x418, 0x408);         // m_gameState.m_unkBool10
inline constexpr std::size_t kLayerLevelFlipping = GDOFF(0x41c, 0x40c);     // m_gameState.m_levelFlipping
inline constexpr std::size_t kLayerDualMode = GDOFF(0x422, 0x412);          // m_gameState.m_isDualMode
inline constexpr std::size_t kLayerTargetGroupsArray = GDOFF(0xf78, 0xfc8); // m_targetGroupsArray
inline constexpr std::size_t kLayerObjectParent = GDOFF(0xfd0, 0x1018);     // m_objectParent
inline constexpr std::size_t kLayerAboveShaderParent = GDOFF(0xfe0, 0x1028); // m_aboveShaderParent
inline constexpr std::size_t kLayerObjectLayer = GDOFF(0xfe8, 0x1030);      // m_objectLayer
inline constexpr std::size_t kLayerInShaderObjectLayer = GDOFF(0xff0, 0x1038);    // m_inShaderObjectLayer
inline constexpr std::size_t kLayerAboveShaderObjectLayer = GDOFF(0xff8, 0x1040); // m_aboveShaderObjectLayer
inline constexpr std::size_t kLayerBatchNodes = GDOFF(0x1028, 0x1070);      // m_batchNodes
inline constexpr std::size_t kLayerVarianceValues = GDOFF(0x10cc, 0x1124);  // m_varianceValues
inline constexpr std::size_t kLayerEnterEasingValues = GDOFF(0x3020, 0x3098); // m_enterEasingValues
inline constexpr std::size_t kLayerShaderLayer = GDOFF(0x3178, 0x31d8);     // m_shaderLayer
inline constexpr std::size_t kLayerRandomSeed = GDOFF(0x32e0, 0x3328);      // m_randomSeed
inline constexpr std::size_t kLayerNonEffectObjects = GDOFF(0x35b0, 0x3598);       // m_nonEffectObjects
inline constexpr std::size_t kLayerCollisionBlockSections = GDOFF(0x35c8, 0x35b0); // m_collisionBlockSections
inline constexpr std::size_t kLayerNonEffectObjectsSizes = GDOFF(0x3658, 0x3640);  // m_nonEffectObjectsSizes
inline constexpr std::size_t kLayerCollisionBlockSectionSizes = GDOFF(0x3670, 0x3658); // m_collisionBlockSectionSizes
inline constexpr std::size_t kLayerMaxGameplayY = GDOFF(0x36a8, 0x3690);    // m_maxGameplayY

// ---- PlayerObject ----
inline constexpr std::size_t kPlayerSpriteWidthScale = GDOFF(0x394, 0x37c);   // m_spriteWidthScale
inline constexpr std::size_t kPlayerSpriteHeightScale = GDOFF(0x398, 0x380);  // m_spriteHeightScale
inline constexpr std::size_t kPlayerSlopeStartTime = GDOFF(0x598, 0x580);     // m_slopeStartTime
inline constexpr std::size_t kPlayerCollisionLogBottom = GDOFF(0x5b8, 0x5a0); // m_collisionLogBottom
inline constexpr std::size_t kPlayerCollisionLogLeft = GDOFF(0x5c0, 0x5a8);   // m_collisionLogLeft
inline constexpr std::size_t kPlayerCurrentSlope2 = GDOFF(0x5e8, 0x5d0);      // m_currentSlope2
inline constexpr std::size_t kPlayerCollidedObject = GDOFF(0x600, 0x5e8);     // m_collidedObject
inline constexpr std::size_t kPlayerLastGroundObject = GDOFF(0x608, 0x5f0);   // m_lastGroundObject
inline constexpr std::size_t kPlayerCollidingWithSlope = GDOFF(0x658, 0x648); // m_isCollidingWithSlope
inline constexpr std::size_t kPlayerCurrentSlope = GDOFF(0x678, 0x668);       // m_currentSlope
inline constexpr std::size_t kPlayerSlopeFlipGravity = GDOFF(0x68c, 0x67c);   // m_slopeFlipGravityRelated
inline constexpr std::size_t kPlayerRotationSpeed = GDOFF(0x720, 0x700);      // m_rotationSpeed
inline constexpr std::size_t kPlayerLastSpiderFlipTime = GDOFF(0x820, 0x800); // m_lastSpiderFlipTime
inline constexpr std::size_t kPlayerAccelerating = GDOFF(0x952, 0x92a);       // m_isAccelerating
inline constexpr std::size_t kPlayerCollidedBottomMaxY = GDOFF(0x960, 0x938); // m_collidedBottomMaxY
inline constexpr std::size_t kPlayerCollidedLeftMaxX = GDOFF(0x968, 0x940);   // m_collidedLeftMaxX
inline constexpr std::size_t kPlayerJumpBuffered = GDOFF(0x985, 0x95d);       // m_jumpBuffered
inline constexpr std::size_t kPlayerStateRingJump = GDOFF(0x986, 0x95e);      // m_stateRingJump
inline constexpr std::size_t kPlayerStateJumpBuffered = GDOFF(0x989, 0x961);  // m_stateJumpBuffered
inline constexpr std::size_t kPlayerStateRingJump2 = GDOFF(0x98a, 0x962);     // m_stateRingJump2
inline constexpr std::size_t kPlayerTouchedRing = GDOFF(0x98b, 0x963);        // m_touchedRing, then
                                                                               // m_touchedCustomRing,
                                                                               // m_touchedGravityPortal
inline constexpr std::size_t kPlayerOnSlope = GDOFF(0x9b0, 0x988);            // m_isOnSlope
inline constexpr std::size_t kPlayerUpsideDownSlope = GDOFF(0x9b8, 0x990);    // m_maybeUpsideDownSlope
inline constexpr std::size_t kPlayerUpsideDown = GDOFF(0x9bf, 0x997);         // m_isUpsideDown
inline constexpr std::size_t kPlayerOnGround = GDOFF(0x9c1, 0x999);           // m_isOnGround
inline constexpr std::size_t kPlayerGoingLeft = GDOFF(0x9c2, 0x99a);          // m_isGoingLeft
inline constexpr std::size_t kPlayerSideways = GDOFF(0x9c3, 0x99b);           // m_isSideways
inline constexpr std::size_t kPlayerDashing = GDOFF(0x9e4, 0x9bc);            // m_isDashing
// The raw byte dump cfg `pflags` prints runs from m_collidedObject to m_scaleXRelatedTime.
inline constexpr std::size_t kPlayerFlagsDumpBegin = GDOFF(0x600, 0x5e8);     // m_collidedObject
inline constexpr std::size_t kPlayerFlagsDumpEnd = GDOFF(0xa20, 0x9f8);       // m_scaleXRelatedTime
inline constexpr std::size_t kPlayerLocked = GDOFF(0xa2a, 0xa02);             // m_isLocked
inline constexpr std::size_t kPlayerPosition = GDOFF(0xa90, 0xa60);           // m_position
inline constexpr std::size_t kPlayerTotalTime = GDOFF(0xaa0, 0xa70);          // m_totalTime
inline constexpr std::size_t kPlayerPlatformer = GDOFF(0xb70, 0xb40);         // m_isPlatformer
inline constexpr std::size_t kPlayerStateNoAutoJump = GDOFF(0xb74, 0xb44);    // m_stateNoAutoJump
inline constexpr std::size_t kPlayerStateDartSlide = GDOFF(0xb78, 0xb48);     // m_stateDartSlide
inline constexpr std::size_t kPlayerStateHitHead = GDOFF(0xb7c, 0xb4c);       // m_stateHitHead
inline constexpr std::size_t kPlayerStateFlipGravity = GDOFF(0xb80, 0xb50);   // m_stateFlipGravity
inline constexpr std::size_t kPlayerGravityMod = GDOFF(0xb84, 0xb54);         // m_gravityMod
inline constexpr std::size_t kPlayerStateForce = GDOFF(0xb88, 0xb58);         // m_stateForce
inline constexpr std::size_t kPlayerOutOfBounds = GDOFF(0xc38, 0xc30);        // m_isOutOfBounds
inline constexpr std::size_t kPlayerIgnoreDamage = GDOFF(0xc44, 0xc3c);       // m_ignoreDamage

// ---- GameObject and subclasses ----
// Android's GameObject members start 4 bytes earlier (0x26c, not 0x270): the Itanium ABI puts
// the first int into CCSpritePlus's tail padding, MSVC does not. Fields up to the first
// 8-aligned pointer keep that shift; later ones re-align (m_glowSprite is +0x2f0 on both).
inline constexpr std::size_t kObjOuterSectionIndex = GDOFF(0x278, 0x274);   // m_outerSectionIndex
inline constexpr std::size_t kObjGroupDisabled = GDOFF(0x28e, 0x28a);       // m_isGroupDisabled
inline constexpr std::size_t kObjPositionXOffset = GDOFF(0x2a0, 0x29c);     // m_positionXOffset
inline constexpr std::size_t kObjPositionYOffset = GDOFF(0x2a4, 0x2a0);     // m_positionYOffset
inline constexpr std::size_t kObjTempOffsetXRelated = GDOFF(0x2c8, 0x2c4);  // m_tempOffsetXRelated
inline constexpr std::size_t kObjGlowSprite = GDOFF(0x2f0, 0x2f0);          // m_glowSprite
inline constexpr std::size_t kObjUniqueID = GDOFF(0x39c, 0x384);            // m_uniqueID
inline constexpr std::size_t kObjVarianceIndex = GDOFF(0x3f4, 0x3dc);       // m_varianceIndex
inline constexpr std::size_t kObjAreaStamp = GDOFF(0x4e0, 0x4c8);           // m_unk4C8
inline constexpr std::size_t kObjPassable = GDOFF(0x515, 0x4fd);            // m_isPassable
inline constexpr std::size_t kObjAreaMoveSkip = GDOFF(0x520, 0x508);        // m_unk508
inline constexpr std::size_t kRingClaimTouch = GDOFF(0x740, 0x71c);         // RingObject::m_claimTouch
inline constexpr std::size_t kCheckpointObject = GDOFF(0xe58, 0xe68);       // CheckpointObject::m_physicalCheckpointObject

// EnterEffectObject (the area trigger)
inline constexpr std::size_t kEnterStartAngle = GDOFF(0x77c, 0x758);        // m_startAngle
inline constexpr std::size_t kEnterAnglePosition = GDOFF(0x780, 0x75c);     // m_anglePosition
inline constexpr std::size_t kEnterRelative = GDOFF(0x788, 0x764);          // m_relative
inline constexpr std::size_t kEnterRelativeFade = GDOFF(0x78c, 0x768);      // m_relativeFade
inline constexpr std::size_t kEnterEasingInType = GDOFF(0x790, 0x76c);      // m_easingInType
inline constexpr std::size_t kEnterEasingInRate = GDOFF(0x794, 0x770);      // m_easingInRate
inline constexpr std::size_t kEnterEasingInBuffer = GDOFF(0x798, 0x774);    // m_easingInBuffer
inline constexpr std::size_t kEnterEasingOutType = GDOFF(0x79c, 0x778);     // m_easingOutType
inline constexpr std::size_t kEnterEasingOutRate = GDOFF(0x7a0, 0x77c);     // m_easingOutRate
inline constexpr std::size_t kEnterEasingOutBuffer = GDOFF(0x7a4, 0x780);   // m_easingOutBuffer
inline constexpr std::size_t kEnterDirectionType = GDOFF(0x7c0, 0x79c);     // m_directionType
inline constexpr std::size_t kEnterXYMode = GDOFF(0x7c4, 0x7a0);            // m_xyMode
inline constexpr std::size_t kEnterEaseOutEnabled = GDOFF(0x7c5, 0x7a1);    // m_easeOutEnabled
inline constexpr std::size_t kEnterInbound = GDOFF(0x7ec, 0x7c8);           // m_inbound
inline constexpr std::size_t kEnterDontEditAreaParent = GDOFF(0x7f5, 0x7d1); // m_dontEditAreaParent

// EnterEffectInstance (not a node; Android's copy is 0x20 longer before these fields)
inline constexpr std::size_t kInstLength = GDOFF(0x10, 0x30);               // m_length
inline constexpr std::size_t kInstLengthVariance = GDOFF(0x14, 0x34);       // m_lengthVariance
inline constexpr std::size_t kInstOffsetVariance = GDOFF(0x1c, 0x3c);       // m_offsetVariance
inline constexpr std::size_t kInstOffsetYVariance = GDOFF(0x24, 0x44);      // m_offsetYVariance
inline constexpr std::size_t kInstModFront = GDOFF(0x28, 0x48);             // m_modFront
inline constexpr std::size_t kInstModBack = GDOFF(0x2c, 0x4c);              // m_modBack
inline constexpr std::size_t kInstDeadzone = GDOFF(0x30, 0x50);             // m_deadzone
inline constexpr std::size_t kInstMoveDistance = GDOFF(0x34, 0x54);         // m_moveDistance
inline constexpr std::size_t kInstMoveDistanceVariance = GDOFF(0x38, 0x58); // m_moveDistanceVariance
inline constexpr std::size_t kInstMoveAngle = GDOFF(0x3c, 0x5c);            // m_moveAngle
inline constexpr std::size_t kInstMoveAngleVariance = GDOFF(0x40, 0x60);    // m_moveAngleVariance
inline constexpr std::size_t kInstMoveX = GDOFF(0x44, 0x64);                // m_moveX
inline constexpr std::size_t kInstMoveXVariance = GDOFF(0x48, 0x68);        // m_moveXVariance
inline constexpr std::size_t kInstMoveY = GDOFF(0x4c, 0x6c);                // m_moveY
inline constexpr std::size_t kInstMoveYVariance = GDOFF(0x50, 0x70);        // m_moveYVariance
inline constexpr std::size_t kInstScaleXVariance = GDOFF(0x5c, 0x7c);       // m_scaleXVariance
inline constexpr std::size_t kInstScaleYVariance = GDOFF(0x64, 0x84);       // m_scaleYVariance
inline constexpr std::size_t kInstRotateVariance = GDOFF(0x74, 0x94);       // m_unk074
inline constexpr std::size_t kInstGameObject = GDOFF(0xa0, 0xc0);           // m_gameObject
inline constexpr std::size_t kInstTargetGroupIndex = GDOFF(0xc0, 0xe0);     // m_targetGroupIndex

// ---- Menu layers (hooks_playmenu.cpp's early-out asserts) ----
inline constexpr std::size_t kLevelInfoBusy = GDOFF(0x1e0, 0x1e0);          // LevelInfoLayer::m_isBusy
inline constexpr std::size_t kLevelInfoTransitionDone = GDOFF(0x275, 0x275); // LevelInfoLayer::m_enterTransitionFinished
inline constexpr std::size_t kLevelPageBusy = GDOFF(0x1a0, 0x1a0);          // LevelPage::m_isBusy
inline constexpr std::size_t kEditLevelExiting = GDOFF(0x1e0, 0x1e0);       // EditLevelLayer::m_exiting
inline constexpr std::size_t kLevelRequiredCoins = GDOFF(0x434, 0x344);     // GJGameLevel::m_requiredCoins

// ---- Static data, as offsets from geode::base::get() ----
// The three 64-bit LCG states (x * 214013 + 2531011) GD never reseeds per attempt except the
// first. Android: the first is GameToolbox::fast_rand's state; the second is the one
// GameObject::resetObject and commonSetup draw the variance index from; the third is the
// one GJBaseGameLayer::init fills m_varianceValues from. geode::base::get() is
// libcocos2dcpp.so's load base on Android, whose first segment has vaddr 0.
inline constexpr std::uintptr_t kSeedTriggerRva = GDOFF(0x6c2e90, 0x1241830);
inline constexpr std::uintptr_t kSeedVarIndexRva = GDOFF(0x6c2ee0, 0x1242080);
inline constexpr std::uintptr_t kSeedVarTableRva = GDOFF(0x6c2ef8, 0x1242da0);

}  // namespace gdoff
