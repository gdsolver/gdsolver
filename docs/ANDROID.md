# Android (android64)

Experimental. The results and solutions published here were measured on Windows (GD 2.2091, Geode 6.0.0) in a 1706x960 (16:9) window. At another resolution or texture quality, or on another platform (Android included), the same level can be solved differently and take a different time.
The model's physics is Windows GD's, and Android GD has not been measured against it.

## What v0.8.1 for Android is

The same source tree as the Windows release, built for **Geometry Dash 2.2091 on Geode 6.0.0, arm64-v8a**
(the Android build whose store version code is 43; `mod.json` names it as `gd.android`). Since v0.8.1 the
release's one `gdsolver.solver.geode` holds both builds (the Windows DLL and `gdsolver.solver.android64.so`): CI builds
each and joins them with `geode package merge`, so there is one file to download whatever the platform. The previous
Android pre-release was v0.5.0, for 2.2081 under Geode 5; v0.6.0 to v0.8.0 had no Android build.

Every raw number the mod reads without a bindings name was measured again on that game's
`libcocos2dcpp.so` (sha256 `700588a06cc17cc963cc480cac4294f5cd55ee4cace062e69383e38c61b3b291`);
they are the android64 column of `src/mod/gd_offsets.hpp` and `src/mod/gd_globals.hpp`, each with where it
was found. `python tools/android_offsets.py --so libcocos2dcpp.so` re-checks all of them against the
bindings' layout and that binary. Nothing the README or the results claim was measured on Android.

What is **not** there on Android:

- **No executable guard.** On Windows the mod refuses to hook a game build it was not made for
  (`exe_guard.hpp`, by PE timestamps). Android has no equivalent: the mod trusts Geode's version check, and the
  raw offsets are right only for the `.so` above. A different build of "2.2091" would be hooked anyway.
- **No crash guard around the draw** (`safeVisit` runs unguarded: there is no SEH).
- **Diagnostics that read Windows byte offsets** (the `cfg` keys that dump a raw object image: `btraw`,
  `btslots`, `ctrace`'s pointer columns, `ctupd`, and a few more) are for the Windows build. They compile on
  Android and read the wrong bytes there; leave them off. The solver itself does not use them.

## A solve on a phone is not the Windows solve

The same level can come out differently on a device, in the plan and in the time it takes. The mod asks the game itself, and what the game on a
device reports is not always what the game on a PC reports. On official level 22, one phone (a 2410x1080 screen) and a Windows worker (a 1706x960
window) gave the solver different object rectangles: 3,092 of the 18,353 rows of the objects' export differed, each by 0.5 to 1 unit. The screen
also differs (the camera's width and left edge, which camera-driven triggers read). Why the rectangles differ has not been pinned down; the
textures' quality is the likely candidate. One dp call, replayed on that phone and on Windows with the same input files, gave the same plan and
trace; that one call is all that was compared. A solution and an iteration count from a device are the device's own and are not comparable
with the Windows ones.

## How it differs from Windows

- **Raw offsets.** Fields the mod reads without a bindings name sit elsewhere on Android: GD there is built
  with libc++, and the Itanium ABI lays out derived classes differently. A member the bindings name is
  `offsetof` of that member on every platform; only the others stay raw, and `gd_offsets_check.cpp` holds
  each raw value to the bindings where the bindings have a name for it.
- **Code GD inlines on one platform only.** Android calls the spider-flip check
  (`PlayerObject::isSafeSpiderFlip`) and the glow removal (`GameObject::removeGlow`) as
  functions, and inlines `moveAreaObject`, `resetAreaObjectValues`, `getAreaObjectValue` and
  `getEasedAreaValue`, which Windows keeps as functions.
- **The area position.** GD reads it through a different vtable slot and returns it through a
  different register.
- **The seeds.** The three random-number states are at other addresses.

## Where the code differs

- `src/mod/gd_offsets.hpp`, `src/mod/gd_globals.hpp`: the raw offsets and seed addresses, as
  `GDOFF(windows 2.2081, windows 2.2091, android64 2.2091)`.
- `src/mod/gd_offsets_check.cpp`: holds each raw offset against the bindings at compile time, and the
  sizes of the classes the mod allocates itself against what the game's own `create()` allocates.
- `src/solver/areaenv.hpp`: `areaPos` calls `getRealPosition()` on Android.
- `src/mod/hooks_gamelayer.cpp`: GD's spawn-channel maps are read as `gd::unordered_map`.
- `src/mod/gd_compat.hpp`, `src/mod/hooks_vtime.cpp`: the Windows calls (`TerminateProcess`, the MSVC return-address
  intrinsics) are Windows-only.
- `tools/android_offsets.py`: re-checks the offsets against the bindings and an Android
  `libcocos2dcpp.so` (`--gd-version` picks the bindings folder; the STL sizes of each Android build are in it).

## Building

The `android` job of `.github/workflows/build.yml` builds it with the SDK of the version
`mod.json` asks for and the same bindings commit as the Windows job; the `package` job merges the two. Locally: the Android NDK r29, Ninja, and

```
cmake -S . -B build-android -G Ninja -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-23 -DGEODE_TARGET_PLATFORM=Android64 \
      -DCMAKE_BUILD_TYPE=Release -DGDSOLVER_BUILD_DP=OFF
cmake --build build-android
```

with `GEODE_SDK` pointing at a Geode 6.0.0 checkout (its `bin/6.0.0/Geode.android64.so` in place). The result is
`gdsolver.solver.geode`, holding `gdsolver.solver.android64.so`.
