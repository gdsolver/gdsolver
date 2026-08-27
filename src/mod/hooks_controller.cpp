// XInput hooks: keep worker processes' hands off the physical controller.
//
// cocos2d polls the pad every frame whether the window is focused or not
// (libcocos2d.dll imports XINPUT1_4.dll ordinal 2 = XInputGetState statically and
// carries a LoadLibrary fallback list xinput1_4/1_3/1_2/1_1/9_1_0 — measured from
// the 2.2081 binaries, 2026-08-26). With a solve fleet running that is two problems
// at once: the user's pad input leaks into every background worker, and a worker
// merely starting up announces itself to the controller stack in the middle of
// whatever game the user is actually playing.
//
// So a worker reports "no controller connected": XInputGetState and
// XInputGetCapabilities return ERROR_DEVICE_NOT_CONNECTED before cocos ever sees a
// device. Every DLL of cocos's own fallback list is pre-loaded and hooked, so the
// lazy dynamic path cannot pick an unhooked copy later. (An instantly-failing
// GetState is also cheaper than the real one, which is known to be slow for absent
// pads.)
//
// The gate is per process, decided once at mod load: a worker data root
// (--geode:gdsolver.data-root=.../worker-N/...) loses the pad, everything else
// keeps it — the mod merely being loaded must not change human play (the same
// principle as the recording block, user decision 2026-08-23). The launch argument
// --geode:gdsolver.controller=off|on overrides in either direction; a human demo
// recorded on a pad inside a worker needs controller=on.
#include "mod/config.hpp"

using namespace p1;

namespace {

DWORD WINAPI xinputGetStateOff(DWORD, void*) { return ERROR_DEVICE_NOT_CONNECTED; }
DWORD WINAPI xinputGetCapabilitiesOff(DWORD, DWORD, void*) { return ERROR_DEVICE_NOT_CONNECTED; }

template <class Detour>
int hookExport(HMODULE mod, const char* dll, const char* func, Detour* detour) {
    auto* p = reinterpret_cast<void*>(GetProcAddress(mod, func));
    if (!p) return 0;
    auto res = Mod::get()->hook(p, detour, std::string(dll) + "!" + func);
    if (res.isErr()) {
        log::error("controller: hooking {}!{} failed: {}", dll, func, res.unwrapErr());
        return 0;
    }
    return 1;
}

}  // namespace

$execute {
    // The decision needs the data root (worker vs local), which is normally resolved at
    // MenuLayer. Resolving it here as well is safe: it is idempotent, and everything it
    // reads (the save dir, the launch arguments) exists before any mod binary loads.
    resolveDataDir();
    bool off = workerTag() != "local";
    if (auto arg = Mod::get()->getLaunchArgument("controller")) {
        if (*arg == "off") off = true;
        else if (*arg == "on") off = false;
        else log::error("controller: ignoring unknown value '{}' (want off|on)", *arg);
    }
    if (!off) return;
    int n = 0;
    for (const char* dll : { "xinput1_4.dll", "xinput1_3.dll", "xinput1_2.dll",
                             "xinput1_1.dll", "xinput9_1_0.dll" }) {
        HMODULE m = GetModuleHandleA(dll);
        if (!m) m = LoadLibraryA(dll);
        if (!m) continue;  // this Windows does not have that generation — nothing to hook
        n += hookExport(m, dll, "XInputGetState", &xinputGetStateOff);
        n += hookExport(m, dll, "XInputGetCapabilities", &xinputGetCapabilitiesOff);
    }
    log::info("controller: disabled for this process ({}), {} XInput entry points hooked",
              workerTag(), n);
    if (n == 0)
        log::warn("controller: no XInput entry point found — the pad is NOT blocked");
}
