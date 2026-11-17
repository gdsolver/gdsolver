# Android (android64)

Experimental and not supported. The model's physics is Windows GD's; whether Android GD
behaves the same is not known, so a result reached on Android is Android's alone.

v0.6.0 and v0.7.0 have no Android build. The Android game is on 2.209 too, and there is no 2.209 port
of the bindings for android64 yet. The last Android pre-release is v0.5.0's, for GD 2.2081
under Geode, and the CI's Android job now runs only when started by hand. Nothing the README
or the results claim was measured on Android.

## How it differs from Windows

- **Raw offsets.** Most fields the mod reads without a bindings name sit elsewhere on Android
  (GD there uses the GNU STL, and the Itanium ABI lays out derived classes differently).
- **Code GD inlines on one platform only.** Android calls the spider-flip check
  (`PlayerObject::isSafeSpiderFlip`) and the glow removal (`GameObject::removeGlow`) as
  functions, and inlines `moveAreaObject`, `resetAreaObjectValues`, `getAreaObjectValue` and
  `getEasedAreaValue`, which Windows keeps as functions.
- **The area position.** GD reads it through a different vtable slot and returns it through a
  different register.
- **The seeds.** The three random-number states are at other addresses.

## Where the code changed

- `src/mod/gd_offsets.hpp`: every raw offset and seed address, as `GDOFF(windows 2.2081, windows 2.209, android64)`.
  The Windows values are the ones used before.
- `src/mod/gd_offsets_check.cpp`: holds each offset against the bindings at compile time.
- `src/solver/areaenv.hpp`: `areaPos` calls `getRealPosition()` on Android.
- `src/mod/hooks_gamelayer.cpp`: GD's spawn-channel maps are read as `gd::unordered_map`.
- `tools/android_offsets.py`: re-checks the offsets against the bindings and an Android
  `libcocos2dcpp.so`.
