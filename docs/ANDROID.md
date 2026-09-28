# Android (android64)

Status: the raw offsets are resolved for android64 and checked; the port as a whole is not
done. Nothing here has been built for Android or run on a device yet. The list of what
remains is at the end.

## The binary

Every Android value was measured on one file:

| | |
|---|---|
| file | `lib/arm64-v8a/libcocos2dcpp.so` from the Play Store build (package `com.robtopx.geometryjump`) |
| GD version | 2.2081 (the bindings for 2.2081 reproduce its layouts; confirm with the package's versionCode, 41) |
| sha256 | `dda3752ab3a912fd2293561e157ecc98eca799056abd9223a331c1c514175df7` |
| build id | `ca621323d54c1d94f5f13a2778ce9c8baa31b9a8` |

It is GD itself and is never committed; keep it in the lab tree. To take it from a phone the
game is installed on, with USB debugging on:

```sh
adb shell pm path com.robtopx.geometryjump        # lists base.apk and the split APKs
adb pull <path of split_config.arm64_v8a.apk>     # or base.apk if there is no ABI split
unzip -j split_config.arm64_v8a.apk lib/arm64-v8a/libcocos2dcpp.so
```

The Lite build is a different program (no editor, other layouts) and cannot stand in for it.

## What the offsets are and where they live

Every offset the mod reads without a bindings name is in `src/mod/gd_offsets.hpp` as
`GDOFF(win, android64)`. The Windows column is the literal the code used before, unchanged, so
the Windows build compiles to the same instructions. `src/mod/gd_offsets_check.cpp` holds each
value against `offsetof` of the member it stands for, on whichever platform is being built.

`python tools/android_offsets.py --so <libcocos2dcpp.so>` re-checks the whole table: it lays
every class out from the bindings for both platforms and must reproduce both columns, finds
each Android value in the functions of the class that owns it, and re-finds the three seeds.
Without `--so` it runs the layout check only. Neither check alone is enough: a wrong value that
lands on the neighbouring field still finds plenty of accesses, and only the layout catches it.

## How the Android values were found

The member order in the bindings is the same on every platform. What moves a member is:

- **The STL types.** GD for Android was built against the GNU STL, which Geode mirrors as
  `gd::` types: `gd::string` is 8 bytes (32 on Windows), `gd::map`/`gd::set` 48 (16),
  `gd::unordered_map` 56 (64), `gd::vector<bool>` 40 (32). `gd::vector` is 24 on both.
- **Tail padding.** The Itanium ABI puts a derived class's first members into the unused tail
  of its base; MSVC does not. This is why GameObject's own members start at `+0x26c` on Android
  and `+0x270` on Windows: the first `int` sits in CCSpritePlus's padding. The 4-byte shift
  lasts until the next 8-aligned pointer re-aligns the layout, so `m_positionXOffset` is `+0x29c`
  against `+0x2a0` while `m_glowSprite` is `+0x2f0` on both. The same rule decides where
  EnhancedGameObject, EffectGameObject, RingObject and EnterEffectObject begin.
- **Platform blocks.** A few members exist only on Android and iOS (`android, ios { }` in the
  bindings): `GJBaseGameLayer::m_allowedButtons`, `m_spawnCount`, `m_spawnAbuse`,
  `PlayerObject::m_collidingBetweenSteps`, and more elsewhere.

The model needs only each root class's starting offset per platform (the cocos2d bases are
not in the bindings). Those came from the Windows literals and from Android code:
`updateMaxGameplayY` stores `m_maxGameplayY` at `+0x3690` (GJBaseGameLayer), and
`addToSection` / `removeObjectFromSection` keep the section indices at `+0x26c..+0x278`
(GameObject). PlayerObject's start is then derived, not measured, and comes out at `+0x540`.
`pushButton` / `releaseButton` set and clear the press latch at `+0x95d/+0x95e`, which is that
start plus the bindings' offset of `m_jumpBuffered`, so the derivation is checked by an
independent measurement.

The disassembly then confirmed the values the solver depends on in the functions the Windows
comments name, with the access width the member's type implies. Examples:

| value | Android evidence |
|---|---|
| free-mode byte `+0x311` | `GJBaseGameLayer::checkCollisions` loads it before clamping |
| out-of-bounds latch `+0xc30` | `checkCollisions` loads it, clears it, stores it |
| velocity-limit exemption `+0x92a` | `PlayerObject::updateJump` reads and clears it (6 sites) |
| fire latches `+0x963..+0x965` | `PlayerObject::ringJump` reads and sets each |
| slope ride `+0x988`, `+0x990`, `+0x668`, `+0x580` | `collidedWithSlopeInternal` |
| section vectors `+0x3598`, `+0x35b0`, sizes `+0x3640`, `+0x3658` | `sortSectionVector`, `addToSection` |
| variance table `+0x1124` | `GJBaseGameLayer::init` fills 2000 floats there |
| EnterEffectInstance fields `+0x30..+0xe0` | `processAreaMoveGroupAction` (the instance is 0x20 longer on Android before them) |
| `m_position` `+0xa60`, lock byte `+0xa02` | `GJBaseGameLayer::update`'s mirror-transition store |
| `GJGameLevel::m_requiredCoins` `+0x344` | `LevelPage::onPlay` |

## Where Android's code differs from Windows

- **The spider rule is a function.** On Windows it is inlined into collidedWithObjectInternal
  (`[+0x820]` held against `[+0xaa0]`). On Android it is `PlayerObject::isSafeSpiderFlip(float)`,
  reading `+0x800` and `+0xa70`, with the 0.04 s threshold passed as its argument. The reads
  `+0xa70` next to `+0xb20` inside collidedWithObjectInternal are a different rule: 0.2 s
  after `m_slopeEndTime`.
- **The checkpoint's glow node.** Windows inlines its release (`object+0x2f0`); Android calls
  `GameObject::removeGlow()`, which does the same with the same field.
- **The area-move position.** `processAreaMoveGroupAction` reads each object's position
  through vtable `+0x540`, which the Android vtable resolves to
  `GameObject::getRealPosition()`. Android returns a `CCPoint` through `x8`, so the Windows
  call (`areaenv::areaPos`: slot `+0x4a8`, the result pointer in `rdx`) cannot be reused as
  written. The Android branch calls the virtual by name.
- **The seeds.** The three never-reseeded LCG states (`x * 214013 + 2531011`) are, on Android:
  `GameToolbox::fast_rand`'s state at `+0x1241830` (the trigger seed; `PlayLayer::resetLevel`
  reseeds it through `fast_srand` and keeps the value at `m_randomSeed`, `+0x3328`), the state
  `GameObject::resetObject` and `commonSetup` draw the variance index from at `+0x1242080`, and
  the state `GJBaseGameLayer::init` fills the variance table from at `+0x1242da0`. Geode's
  `base::get()` is libcocos2dcpp.so's load base on Android and the file's first segment has
  vaddr 0, so these are used the way the Windows RVAs are.

## What remains before the mod runs on Android

1. **Two hooks have no Android function.** `GJBaseGameLayer::moveAreaObject` and
   `resetAreaObjectValues` (hooks_area.cpp, the area-move envelope) are inlined on Android;
   there are no symbols to hook. `getAreaObjectValue` and `getEasedAreaValue`, which
   areaenv.hpp reproduces, are inlined too. The envelope needs another hook point, most likely
   `processAreaMoveGroupAction` itself.
2. **Windows-only code.** Structured exception handling (`__try` in playlayer_helpers.hpp,
   hooks_playlayer.cpp, hooks_gamelayer.cpp), the crash handler and stack scan in
   postmortem.hpp (`SetUnhandledExceptionFilter`, `VirtualQuery`), `RtlCaptureStackBackTrace`
   in hooks_player.cpp, `K32GetProcessMemoryInfo` in hooks_gamelayer.cpp and postmortem.hpp,
   `wglGetProcAddress` in hooks_menu.cpp, `GetProcAddress` in hooks_controller.cpp, and
   `_mm_getcsr` (x86 only) in repair.hpp and dp/src/dp/cli.hpp. Each needs an Android
   equivalent or a guard. The SEH blocks are the ones with consequences: they turn a fault
   inside GD into a recorded miss, and Android has no direct counterpart.
3. **psnap's member table.** `src/po_members.inc` is generated from the Windows member set.
   The Android-only `PlayerObject::m_collidingBetweenSteps` is not in it, so a snapshot
   restore on Android would leave that byte as it was.
4. **Floating point.** The acceptance criteria are byte-identical solver output. AArch64
   compilers contract `a * b + c` into a fused multiply-add unless told not to, which changes
   the last bits relative to x64. The solver core needs `-ffp-contract=off` (and a check that
   the Android CLI reproduces the Windows plans) before any Android result can be compared with
   a Windows baseline. GD's own ARM build fuses where its compiler chose to, so the model's
   agreement with the game has to be re-measured on Android, not assumed.
5. **Build and packaging.** `mod.json` declares `gd.win` only, and CI builds Windows only. An
   Android build needs the NDK (Geode requires API level 23 or higher) and an `android64`
   entry in both.
