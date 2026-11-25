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
// A new raw offset goes here, with all three values. A platform without values stops the build.
//
// The win209 column was written before 2.209's Geode bindings existed: it comes from matching the
// functions of the two Windows binaries and comparing the member accesses in each pair. The
// static_asserts in gd_offsets_check.cpp are what verifies it -- build against the 2.209 bindings, and
// every value that is wrong stops the build. Values that rest on a single vote, or on none (the
// neighbours' shift or the 2.2081 value carried over):
//   kLayerSpawnChannel1: 0x388 -> 0x388 (weak)
//   kLayerInShaderObjectLayer: 0xff0 -> 0x1110 (weak)
//   kLayerRandomSeed: 0x32e0 -> 0x3420 (weak)
//   kLayerGateMap: 0x4f0 -> 0x4f0 (weak; read off 2.209b only)
//   kPlayerSpriteWidthScale: 0x394 -> 0x394 (weak)
//   kPlayerSpriteHeightScale: 0x398 -> 0x398 (weak)
//   kPlayerOutOfBounds: 0xc38 -> 0xc40 (inferred)
//   kObjGroupDisabled: 0x28e -> 0x28e (weak)
//   kObjPositionYOffset: 0x2a4 -> 0x2a4 (weak)
//   kObjVarianceIndex: 0x3f4 -> 0x3f4 (weak)
//   kEnterStartAngle: 0x77c -> 0x77c (inferred)
//   kEnterAnglePosition: 0x780 -> 0x780 (weak)
//   kEnterRelative: 0x788 -> 0x788 (weak)
//   kEnterEasingInBuffer: 0x798 -> 0x798 (weak)
//   kEnterEasingOutType: 0x79c -> 0x79c (weak)
//   kEnterEasingOutRate: 0x7a0 -> 0x7a0 (weak)
//   kEnterEasingOutBuffer: 0x7a4 -> 0x7a4 (inferred)
//   kEnterDirectionType: 0x7c0 -> 0x7c0 (weak)
//   kEnterXYMode: 0x7c4 -> 0x7c4 (weak)
//   kEnterEaseOutEnabled: 0x7c5 -> 0x7c5 (weak)
//   kEnterInbound: 0x7ec -> 0x7ec (weak)
//   kEnterDontEditAreaParent: 0x7f5 -> 0x7f5 (weak)
//   kInstLengthVariance: 0x14 -> 0x14 (weak)
//   kInstOffsetVariance: 0x1c -> 0x1c (weak)
//   kInstMoveAngleVariance: 0x40 -> 0x40 (weak)
//   kInstMoveX: 0x44 -> 0x44 (weak)
//   kInstMoveXVariance: 0x48 -> 0x48 (weak)
//   kInstMoveY: 0x4c -> 0x4c (weak)
//   kInstScaleXVariance: 0x5c -> 0x5c (weak)
//   kInstScaleYVariance: 0x64 -> 0x64 (weak)
//   kInstRotateVariance: 0x74 -> 0x74 (weak)
//   kInstGameObject: 0xa0 -> 0xa0 (unverified)
//   kInstTargetGroupIndex: 0xc0 -> 0xc0 (unverified)
// ============================================================
#include <Geode/Geode.hpp>

#include <cstddef>
#include <cstdint>

// GDOFF(win2081, win209, android64): the Windows value for GD 2.2081, the Windows value for GD 2.209 and
// later (GEODE_COMP_GD_VERSION >= 22090), and the android64 value, which is 2.2081's -- there is no 2.209
// Android binary here, so an Android build of 2.209 stops rather than read 2.2081's layout.
//
// 2.209 exists as two builds of the executable: 524265B3 (2026-10-13) and the hotfix 4EEE06D5 (2026-10-17, Steam build
// 26002420), which moved every function and took 0x40 bytes out of GJBaseGameLayer at m_effectManager. A build for the
// first has GEODE_COMP_GD_VERSION 22090, one for the second 22091 (the bindings branch is the other half of the choice).
// GD209(earlier, update) is the win209 value of each: a constant that did not move is written once.
//
// THE STATIC_ASSERTS ARE NOT THE PROOF. gd_offsets_check.cpp compares this header with the bindings, and both can carry the same stale number: on
// the update (4EEE06D5) LevelInfoLayer gained an 8-byte member after m_unk2, both still said 0x27d for m_enterTransitionFinished, every assert passed, and
// the Play menu's early-out read a byte of m_orbsIcon (byte 5 of the sprite's address: zero whenever that address is below 2^40, then no menu and the level just started). A value is right when the
// EXECUTABLE says so: the same code in both builds must use the number, in the function that uses the member. The check that was made for the whole
// header (107 of its 122 values read from both executables, one wrong, 14 not read by any function) is the lab's layout_scan.py + gd_offsets_verify.py,
// which need both game executables and so cannot be a test here; re-run them when a new build of the game appears. And a change in a class's layout
// that this header has no value for (a member the mod names through the bindings) is the bindings' to carry: the PAD in bindings/2.209 for the update.
#if GEODE_COMP_GD_VERSION >= 22091
#define GD209(earlier, update) (update)
#else
#define GD209(earlier, update) (earlier)
#endif
#if defined(GEODE_IS_WINDOWS)
#if GEODE_COMP_GD_VERSION >= 22090
#define GDOFF(win, win209, android64) (win209)
#define GD_WIN_LITERAL(win, win209) (win209)
#else
#define GDOFF(win, win209, android64) (win)
#define GD_WIN_LITERAL(win, win209) (win)
#endif
#elif defined(GEODE_IS_ANDROID64)
#if GEODE_COMP_GD_VERSION >= 22090
#error "gd_offsets.hpp: no android64 offsets for GD 2.209 (measure them on that binary first)"
#endif
#define GDOFF(win, win209, android64) (android64)
#define GD_WIN_LITERAL(win, win209) (win)
#else
#error "gd_offsets.hpp: no raw offsets for this platform (Windows and android64 only)"
#endif

namespace gdoff {

// Named members: the offset is the bindings' own (Geode 6's bindings for GD 2.2091), not a number of ours.
inline constexpr std::size_t kGameState = offsetof(GJBaseGameLayer, m_gameState);
#define GDOFF_STATE(m) (kGameState + offsetof(GJGameState, m))
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#endif

// ---- GJBaseGameLayer (m_gameState is embedded at +0x1a8 on both) ----
inline constexpr std::size_t kLayerCameraZoom = GDOFF_STATE(m_cameraZoom);        // m_gameState.m_cameraZoom
inline constexpr std::size_t kLayerCameraOffsetY = GDOFF_STATE(m_cameraOffset) + offsetof(cocos2d::CCPoint, y);     // m_gameState.m_cameraOffset.y
inline constexpr std::size_t kLayerUnkPoint17Y = GDOFF(0x23c, 0x23c, 0x23c);       // m_gameState.m_unkPoint17.y
inline constexpr std::size_t kLayerUnkBool1 = GDOFF_STATE(m_dualStaticCamera);          // m_gameState.m_unkBool1
inline constexpr std::size_t kLayerFreeMode = GDOFF_STATE(m_isFreeMode);          // m_gameState.m_isFreeMode
inline constexpr std::size_t kLayerCurrentChannel = GDOFF_STATE(m_currentChannel);    // m_gameState.m_currentChannel
inline constexpr std::size_t kLayerSpawnChannel0 = GDOFF_STATE(m_spawnChannelRelated0);     // m_gameState.m_spawnChannelRelated0
inline constexpr std::size_t kLayerSpawnChannel1 = GDOFF_STATE(m_spawnChannelRelated1);     // m_gameState.m_spawnChannelRelated1
inline constexpr std::size_t kLayerUnkBool10 = GDOFF_STATE(m_isFlipped);         // m_gameState.m_unkBool10
inline constexpr std::size_t kLayerLevelFlipping = GDOFF_STATE(m_levelFlipProgress);     // m_gameState.m_levelFlipping
inline constexpr std::size_t kLayerDualMode = GDOFF_STATE(m_isDualMode);          // m_gameState.m_isDualMode
inline constexpr std::size_t kLayerTargetGroupsArray = offsetof(GJBaseGameLayer, m_targetGroupsArray); // m_targetGroupsArray
inline constexpr std::size_t kLayerObjectParent = offsetof(GJBaseGameLayer, m_objectParent);     // m_objectParent
inline constexpr std::size_t kLayerAboveShaderParent = offsetof(GJBaseGameLayer, m_aboveShaderParent); // m_aboveShaderParent
inline constexpr std::size_t kLayerObjectLayer = offsetof(GJBaseGameLayer, m_objectLayer);      // m_objectLayer
inline constexpr std::size_t kLayerInShaderObjectLayer = offsetof(GJBaseGameLayer, m_inShaderObjectLayer);    // m_inShaderObjectLayer
inline constexpr std::size_t kLayerAboveShaderObjectLayer = offsetof(GJBaseGameLayer, m_aboveShaderObjectLayer); // m_aboveShaderObjectLayer
inline constexpr std::size_t kLayerBatchNodes = offsetof(GJBaseGameLayer, m_batchNodes);      // m_batchNodes
inline constexpr std::size_t kLayerVarianceValues = offsetof(GJBaseGameLayer, m_varianceValues);  // m_varianceValues
inline constexpr std::size_t kLayerEnterEasingValues = offsetof(GJBaseGameLayer, m_enterEasingValues); // m_enterEasingValues
inline constexpr std::size_t kLayerShaderLayer = offsetof(GJBaseGameLayer, m_shaderLayer);     // m_shaderLayer
// The once-per-contact gate's std::map<std::pair<int,int>, int> (key: the object's m_uniqueID and the player's, value: the layer's +0x3e0 counter when the pair was last seen), which FUN_14031ef50 reads at layer+0x4f0:
// read off the 2.209b decompilation (11-14); the other builds carry the value over (weak: no named member to assert it against).
inline constexpr std::size_t kLayerGateMap = GDOFF(0x4f0, 0x4f0, 0x4f0);
inline constexpr std::size_t kLayerRandomSeed = offsetof(GJBaseGameLayer, m_randomSeed);      // m_randomSeed
inline constexpr std::size_t kLayerNonEffectObjects = offsetof(GJBaseGameLayer, m_nonEffectObjects);       // m_nonEffectObjects
inline constexpr std::size_t kLayerCollisionBlockSections = offsetof(GJBaseGameLayer, m_collisionBlockSections); // m_collisionBlockSections
inline constexpr std::size_t kLayerNonEffectObjectsSizes = offsetof(GJBaseGameLayer, m_nonEffectObjectsSizes);  // m_nonEffectObjectsSizes
inline constexpr std::size_t kLayerCollisionBlockSectionSizes = GDOFF(0x3670, GD209(0x37b0, 0x3770), 0x3658); // m_collisionBlockSectionSizes
inline constexpr std::size_t kLayerMaxGameplayY = offsetof(GJBaseGameLayer, m_maxGameplayY);    // m_maxGameplayY

// ---- PlayerObject ----
inline constexpr std::size_t kPlayerSpriteWidthScale = offsetof(PlayerObject, m_spriteWidthScale);   // m_spriteWidthScale
inline constexpr std::size_t kPlayerSpriteHeightScale = offsetof(PlayerObject, m_spriteHeightScale);  // m_spriteHeightScale
inline constexpr std::size_t kPlayerSlopeStartTime = offsetof(PlayerObject, m_slopeStartTime);     // m_slopeStartTime
inline constexpr std::size_t kPlayerCollisionLogBottom = offsetof(PlayerObject, m_collisionLogBottom); // m_collisionLogBottom
inline constexpr std::size_t kPlayerCollisionLogLeft = offsetof(PlayerObject, m_collisionLogLeft);   // m_collisionLogLeft
inline constexpr std::size_t kPlayerCurrentSlope2 = offsetof(PlayerObject, m_currentSlope2);      // m_currentSlope2
inline constexpr std::size_t kPlayerCollidedObject = offsetof(PlayerObject, m_collidedObject);     // m_collidedObject
inline constexpr std::size_t kPlayerLastGroundObject = offsetof(PlayerObject, m_lastGroundObject);   // m_lastGroundObject
inline constexpr std::size_t kPlayerCollidingWithSlope = offsetof(PlayerObject, m_isCollidingWithSlope); // m_isCollidingWithSlope
inline constexpr std::size_t kPlayerCurrentSlope = offsetof(PlayerObject, m_currentSlope);       // m_currentSlope
inline constexpr std::size_t kPlayerSlopeFlipGravity = offsetof(PlayerObject, m_slopeIsDownhill);   // m_slopeFlipGravityRelated
inline constexpr std::size_t kPlayerRotationSpeed = offsetof(PlayerObject, m_rotationSpeed);      // m_rotationSpeed
// The ball's ground-stake byte, the one right after m_rotationSpeed (the double above): runBallRotation raises it, toggleRollMode
// clears it, togglePlayerScale re-stakes the ball only while it is up (dp State::ball728). No bindings name; the 2.209 and Android
// values follow m_rotationSpeed's shift and are checked against the dump's b728 column.
inline constexpr std::size_t kPlayerBallStake = GDOFF(0x728, 0x730, 0x708);
inline constexpr std::size_t kPlayerLastSpiderFlipTime = offsetof(PlayerObject, m_lastSpiderFlipTime); // m_lastSpiderFlipTime
inline constexpr std::size_t kPlayerAccelerating = offsetof(PlayerObject, m_isAccelerating);       // m_isAccelerating
inline constexpr std::size_t kPlayerCollidedBottomMaxY = offsetof(PlayerObject, m_collidedBottomMaxY); // m_collidedBottomMaxY
inline constexpr std::size_t kPlayerCollidedLeftMaxX = offsetof(PlayerObject, m_collidedLeftMaxX);   // m_collidedLeftMaxX
inline constexpr std::size_t kPlayerJumpBuffered = offsetof(PlayerObject, m_jumpBuffered);       // m_jumpBuffered
inline constexpr std::size_t kPlayerStateRingJump = offsetof(PlayerObject, m_stateRingJump);      // m_stateRingJump
inline constexpr std::size_t kPlayerStateJumpBuffered = offsetof(PlayerObject, m_stateJumpBuffered);  // m_stateJumpBuffered
inline constexpr std::size_t kPlayerStateRingJump2 = offsetof(PlayerObject, m_statePreJump);     // m_stateRingJump2
inline constexpr std::size_t kPlayerTouchedRing = offsetof(PlayerObject, m_touchedRing);        // m_touchedRing, then
                                                                               // m_touchedCustomRing,
                                                                               // m_touchedGravityPortal
inline constexpr std::size_t kPlayerOnSlope = offsetof(PlayerObject, m_isOnSlope);            // m_isOnSlope
inline constexpr std::size_t kPlayerUpsideDownSlope = offsetof(PlayerObject, m_slopeResnapped);    // m_maybeUpsideDownSlope
inline constexpr std::size_t kPlayerUpsideDown = offsetof(PlayerObject, m_isUpsideDown);         // m_isUpsideDown
inline constexpr std::size_t kPlayerOnGround = offsetof(PlayerObject, m_isOnGround);           // m_isOnGround
inline constexpr std::size_t kPlayerGoingLeft = offsetof(PlayerObject, m_isGoingLeft);          // m_isGoingLeft
inline constexpr std::size_t kPlayerSideways = offsetof(PlayerObject, m_isSideways);           // m_isSideways
inline constexpr std::size_t kPlayerDashing = offsetof(PlayerObject, m_isDashing);            // m_isDashing
// The raw byte dump cfg `pflags` prints runs from m_collidedObject to m_scaleXRelatedTime.
inline constexpr std::size_t kPlayerFlagsDumpBegin = offsetof(PlayerObject, m_collidedObject);     // m_collidedObject
inline constexpr std::size_t kPlayerFlagsDumpEnd = offsetof(PlayerObject, m_lastGroundXDeltaTime);       // m_scaleXRelatedTime
inline constexpr std::size_t kPlayerLocked = offsetof(PlayerObject, m_isLocked);             // m_isLocked
inline constexpr std::size_t kPlayerPosition = offsetof(PlayerObject, m_position);           // m_position
inline constexpr std::size_t kPlayerTotalTime = offsetof(PlayerObject, m_totalTime);          // m_totalTime
inline constexpr std::size_t kPlayerPlatformer = offsetof(PlayerObject, m_isPlatformer);         // m_isPlatformer
inline constexpr std::size_t kPlayerStateNoAutoJump = offsetof(PlayerObject, m_stateNoAutoJump);    // m_stateNoAutoJump
inline constexpr std::size_t kPlayerStateDartSlide = offsetof(PlayerObject, m_stateDartSlide);     // m_stateDartSlide
inline constexpr std::size_t kPlayerStateHitHead = offsetof(PlayerObject, m_stateHitHead);       // m_stateHitHead
inline constexpr std::size_t kPlayerStateFlipGravity = offsetof(PlayerObject, m_stateFlipGravity);   // m_stateFlipGravity
inline constexpr std::size_t kPlayerGravityMod = offsetof(PlayerObject, m_gravityMod);         // m_gravityMod
inline constexpr std::size_t kPlayerStateForce = offsetof(PlayerObject, m_stateForce);         // m_stateForce
inline constexpr std::size_t kPlayerOutOfBounds = offsetof(PlayerObject, m_isOutOfBounds);        // m_isOutOfBounds
inline constexpr std::size_t kPlayerIgnoreDamage = offsetof(PlayerObject, m_ignoreDamage);       // m_ignoreDamage

// ---- GameObject and subclasses ----
// Android's GameObject members start 4 bytes earlier (0x26c, not 0x270): the Itanium ABI puts
// the first int into CCSpritePlus's tail padding, MSVC does not. Fields up to the first
// 8-aligned pointer keep that shift; later ones re-align (m_glowSprite is +0x2f0 on both).
inline constexpr std::size_t kObjOuterSectionIndex = offsetof(GameObject, m_outerSectionIndex);   // m_outerSectionIndex
inline constexpr std::size_t kObjGroupDisabled = offsetof(GameObject, m_isGroupDisabled);       // m_isGroupDisabled
inline constexpr std::size_t kObjPositionXOffset = offsetof(GameObject, m_positionXOffset);     // m_positionXOffset
inline constexpr std::size_t kObjPositionYOffset = offsetof(GameObject, m_positionYOffset);     // m_positionYOffset
inline constexpr std::size_t kObjTempOffsetXRelated = offsetof(GameObject, m_tempOffsetXRelated);  // m_tempOffsetXRelated
inline constexpr std::size_t kObjGlowSprite = offsetof(GameObject, m_glowSprite);          // m_glowSprite
inline constexpr std::size_t kObjUniqueID = offsetof(GameObject, m_uniqueID);            // m_uniqueID
inline constexpr std::size_t kObjVarianceIndex = offsetof(GameObject, m_varianceIndex);       // m_varianceIndex
inline constexpr std::size_t kObjAreaStamp = offsetof(GameObject, m_unk4C8);           // m_unk4C8
inline constexpr std::size_t kObjPassable = offsetof(GameObject, m_isPassable);            // m_isPassable
inline constexpr std::size_t kObjAreaMoveSkip = offsetof(GameObject, m_unk508);        // m_unk508
inline constexpr std::size_t kRingClaimTouch = offsetof(RingObject, m_claimTouch);         // RingObject::m_claimTouch
inline constexpr std::size_t kCheckpointObject = offsetof(CheckpointObject, m_physicalCheckpointObject);       // CheckpointObject::m_physicalCheckpointObject

// EnterEffectObject (the area trigger)
inline constexpr std::size_t kEnterStartAngle = offsetof(EnterEffectObject, m_startAngle);        // m_startAngle
inline constexpr std::size_t kEnterAnglePosition = offsetof(EnterEffectObject, m_anglePosition);     // m_anglePosition
inline constexpr std::size_t kEnterRelative = offsetof(EnterEffectObject, m_relative);          // m_relative
inline constexpr std::size_t kEnterRelativeFade = offsetof(EnterEffectObject, m_relativeFade);      // m_relativeFade
inline constexpr std::size_t kEnterEasingInType = offsetof(EnterEffectObject, m_easingInType);      // m_easingInType
inline constexpr std::size_t kEnterEasingInRate = offsetof(EnterEffectObject, m_easingInRate);      // m_easingInRate
inline constexpr std::size_t kEnterEasingInBuffer = offsetof(EnterEffectObject, m_easingInBuffer);    // m_easingInBuffer
inline constexpr std::size_t kEnterEasingOutType = offsetof(EnterEffectObject, m_easingOutType);     // m_easingOutType
inline constexpr std::size_t kEnterEasingOutRate = offsetof(EnterEffectObject, m_easingOutRate);     // m_easingOutRate
inline constexpr std::size_t kEnterEasingOutBuffer = offsetof(EnterEffectObject, m_easingOutBuffer);   // m_easingOutBuffer
inline constexpr std::size_t kEnterDirectionType = offsetof(EnterEffectObject, m_directionType);     // m_directionType
inline constexpr std::size_t kEnterXYMode = offsetof(EnterEffectObject, m_xyMode);            // m_xyMode
inline constexpr std::size_t kEnterEaseOutEnabled = offsetof(EnterEffectObject, m_easeOutEnabled);    // m_easeOutEnabled
inline constexpr std::size_t kEnterInbound = offsetof(EnterEffectObject, m_inbound);           // m_inbound
inline constexpr std::size_t kEnterDontEditAreaParent = offsetof(EnterEffectObject, m_dontEditAreaParent); // m_dontEditAreaParent

// EnterEffectInstance (not a node; Android's copy is 0x20 longer before these fields)
inline constexpr std::size_t kInstLength = offsetof(EnterEffectInstance, m_length);               // m_length
inline constexpr std::size_t kInstLengthVariance = offsetof(EnterEffectInstance, m_lengthVariance);       // m_lengthVariance
inline constexpr std::size_t kInstOffsetVariance = offsetof(EnterEffectInstance, m_offsetVariance);       // m_offsetVariance
inline constexpr std::size_t kInstOffsetYVariance = offsetof(EnterEffectInstance, m_offsetYVariance);      // m_offsetYVariance
inline constexpr std::size_t kInstModFront = offsetof(EnterEffectInstance, m_modFront);             // m_modFront
inline constexpr std::size_t kInstModBack = offsetof(EnterEffectInstance, m_modBack);              // m_modBack
inline constexpr std::size_t kInstDeadzone = offsetof(EnterEffectInstance, m_deadzone);             // m_deadzone
inline constexpr std::size_t kInstMoveDistance = offsetof(EnterEffectInstance, m_moveDistance);         // m_moveDistance
inline constexpr std::size_t kInstMoveDistanceVariance = offsetof(EnterEffectInstance, m_moveDistanceVariance); // m_moveDistanceVariance
inline constexpr std::size_t kInstMoveAngle = offsetof(EnterEffectInstance, m_moveAngle);            // m_moveAngle
inline constexpr std::size_t kInstMoveAngleVariance = offsetof(EnterEffectInstance, m_moveAngleVariance);    // m_moveAngleVariance
inline constexpr std::size_t kInstMoveX = offsetof(EnterEffectInstance, m_moveX);                // m_moveX
inline constexpr std::size_t kInstMoveXVariance = offsetof(EnterEffectInstance, m_moveXVariance);        // m_moveXVariance
inline constexpr std::size_t kInstMoveY = offsetof(EnterEffectInstance, m_moveY);                // m_moveY
inline constexpr std::size_t kInstMoveYVariance = offsetof(EnterEffectInstance, m_moveYVariance);        // m_moveYVariance
inline constexpr std::size_t kInstScaleXVariance = offsetof(EnterEffectInstance, m_scaleXVariance);       // m_scaleXVariance
inline constexpr std::size_t kInstScaleYVariance = offsetof(EnterEffectInstance, m_scaleYVariance);       // m_scaleYVariance
inline constexpr std::size_t kInstRotateVariance = offsetof(EnterEffectInstance, m_unusedKey285);       // m_unk074
inline constexpr std::size_t kInstGameObject = offsetof(EnterEffectInstance, m_gameObject);           // m_gameObject
inline constexpr std::size_t kInstTargetGroupIndex = offsetof(EnterEffectInstance, m_targetGroupIndex);     // m_targetGroupIndex

// ---- Menu layers (hooks_playmenu.cpp's early-out asserts) ----
inline constexpr std::size_t kLevelInfoBusy = offsetof(LevelInfoLayer, m_isBusy);          // LevelInfoLayer::m_isBusy
inline constexpr std::size_t kLevelInfoTransitionDone = offsetof(LevelInfoLayer, m_enterTransitionFinished); // LevelInfoLayer::m_enterTransitionFinished
inline constexpr std::size_t kLevelPageBusy = offsetof(LevelPage, m_isBusy);          // LevelPage::m_isBusy
inline constexpr std::size_t kEditLevelExiting = offsetof(EditLevelLayer, m_exiting);       // EditLevelLayer::m_exiting
inline constexpr std::size_t kLevelRequiredCoins = offsetof(GJGameLevel, m_requiredCoins);     // GJGameLevel::m_requiredCoins
#if defined(GEODE_IS_WINDOWS) && GEODE_COMP_GD_VERSION >= 22090
// GJGameLevel's practice-mode attempt counter, which 2.209 added and the bindings do not name. Read off the 2.209 hotfix executable (22091): PlayLayer::resetLevel
// (0x518c00) increments it inline while practice mode is on (0x519970: `inc dword ptr [rax+0x6ac]`, rax = m_level, gated on PlayLayer+0x32e0), and
// GJGameLevel::encodeWithCoder (0x2690c0) saves it as key k154 only when it is above zero (0x26916a), so it is part of the level's record. 2.2081 has no such field.
inline constexpr std::size_t kLevelPracticeAttempts = 0x6ac;
#endif

// ---- Static data, as offsets from geode::base::get() ----
// The three 64-bit LCG states (x * 214013 + 2531011) GD never reseeds per attempt except the
// first. Android: the first is GameToolbox::fast_rand's state; the second is the one
// GameObject::resetObject and commonSetup draw the variance index from; the third is the
// one GJBaseGameLayer::init fills m_varianceValues from. geode::base::get() is
// libcocos2dcpp.so's load base on Android, whose first segment has vaddr 0.

// ---- raw literals that were in the code, not in this header (Windows; the same literal everywhere else) ----
// GD_WIN_LITERAL(2.2081, 2.209). The sites keep the value they had on every platform but Windows 2.209.
inline constexpr std::size_t kPlayLayerEndLatch = GD_WIN_LITERAL(0x3878, GD209(0x3a68, 0x3a28));            // checkForEnd's latch byte (PlayLayer)
inline constexpr std::size_t kLayerLevelEndAnimStarted = GD_WIN_LITERAL(0x3570, GD209(0x36b0, 0x3670));     // m_levelEndAnimationStarted
inline constexpr std::size_t kLayerSectionSizeX = GD_WIN_LITERAL(0x36a0, GD209(0x37e0, 0x37a0));            // float, the section cell width cpfollowsection divides by
inline constexpr std::size_t kLayerSectionSizeY = GD_WIN_LITERAL(0x36a4, GD209(0x37e4, 0x37a4));            // float, the cell height
inline constexpr std::size_t kPlayerPendingCheckpoint = GD_WIN_LITERAL(0x890, 0x898);        // PlayerObject::m_pendingCheckpoint
inline constexpr std::size_t kObjPositionVtblSlot = GD_WIN_LITERAL(0x4a8, 0x4b8);            // GameObject's vtable slot of getRealPosition (CCNode gained two virtuals)

// ---- 22091, Windows only: what GJBaseGameLayer::processFollowActions (22091 base+0x339100) reads and writes (cfg follow=1, hooks_area.cpp; read only, nothing is written back) ----------------------------------
// Offsets from that function's decompile, none of them named by the pinned bindings; no value for another platform or version (the follow rows are not written there). The node is a CCMoveCNode.
#if defined(GEODE_IS_WINDOWS) && GEODE_COMP_GD_VERSION >= 22091
inline constexpr std::size_t kEffMgrFollowNodes = 0x7b0;   // GJEffectManager: begin pointer of a vector<CCMoveCNode*> (end pointer at +8)
inline constexpr std::size_t kNodeDeltaX = 0x38;           // double: (leader x - its last x) * xmod, the function's output; kNodeDeltaY = 0x40
inline constexpr std::size_t kNodeDeltaY = 0x40;
inline constexpr std::size_t kNodeCenter = 0x74;           // int: the centre group the leader comes from
inline constexpr std::size_t kNodeFlag = 0x78;             // byte the function tests to take the move branch when both deltas are zero
inline constexpr std::size_t kNodeXMod = 0x80;             // double; kNodeYMod = 0x88
inline constexpr std::size_t kNodeYMod = 0x88;
inline constexpr std::size_t kNodeCommand = 0xc8;          // GroupCommandObject2* (m_groupObject)
inline constexpr std::size_t kObjRealX = 0x3b8;            // GameObject double: the x the function reads for the leader; kObjRealY = 0x3c0
inline constexpr std::size_t kObjRealY = 0x3c0;
inline constexpr std::size_t kObjLastX = 0x4d0;            // GameObject float: the x it subtracts (set from kObjRealX once per frame); kObjLastY = 0x4d4
inline constexpr std::size_t kObjLastY = 0x4d4;
inline constexpr std::size_t kObjLastFrame = 0x4dc;        // int: the frame counter value kObjLastX / Y were taken at
inline constexpr std::size_t kLayerFrameCounter = 0x3e0;   // GJBaseGameLayer int (param_1[0x7c]): the counter kObjLastFrame is compared with
#endif
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#undef GDOFF_STATE

#include "mod/gd_globals.hpp"
}  // namespace gdoff
