# Android (android64)

Experimental and not supported. The model's physics is Windows GD's; whether Android GD
behaves the same is not known, so a result reached on Android is Android's alone.

v0.6.0, v0.7.0 and v0.8.0 have no Android build. v0.8.0 is Windows only (GD 2.2091, Geode 6.0.0); the Android
version follows the Windows release, and needs the android64 values measured again on the Android game it
targets (gd_offsets.hpp stops an Android build of 2.209 or later until then). The last Android pre-release is
v0.5.0's, for GD 2.2081 under Geode 5, and the CI's Android job runs only when started by hand. Nothing the README
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

- `src/mod/gd_offsets.hpp`: the raw offsets and seed addresses, as `GDOFF(windows 2.2081, windows 2.209, android64)`.
  Since v0.8.0 a member the bindings name is `offsetof` of that member on every platform (the earlier android64
  values for those are in the history of v0.5.0 to v0.7.0); only members the bindings do not name stay raw.
- `src/mod/gd_offsets_check.cpp`: holds each raw offset against the bindings at compile time.
- `src/solver/areaenv.hpp`: `areaPos` calls `getRealPosition()` on Android.
- `src/mod/hooks_gamelayer.cpp`: GD's spawn-channel maps are read as `gd::unordered_map`.
- `tools/android_offsets.py`: re-checks the offsets against the bindings and an Android
  `libcocos2dcpp.so`.
