// The keyboard path the mod's keybind settings need on GD 2.209.
//
// The mod's hotkeys (overlay, pause, step, speed, seek, ...) are Geode keybind settings. Geode delivers them
// from its own hook on CCKeyboardDispatcher::dispatchKeyboardMSG, which posts a KeyboardInputEvent that the
// keybind settings listen to. That hook is part of Geode.dll, built for 2.2081, and Geode's forward-compatibility
// mode on 2.209 does not install it: no key ever reaches a keybind setting, so none of the hotkeys worked
// (measured: F2 / F1 / Up pressed for real in a replay changed nothing, while Escape closed the play menu).
// This is the same hook, written in the mod, where it is compiled against this mod's bindings. It posts the
// event and, as Geode's does, lets a listener that takes the key stop it.
//
// Not built for 2.2081: Geode's own hook is there, and two would deliver every key twice.
// Not built against Geode 6 either: it supports 2.209 itself and posts the KeyboardInputEvent from its window
// procedure (loader/src/platform/windows/input.cpp), then calls this dispatcher. A second post from here
// would deliver every key twice, and the hotkeys are the ones that must not run twice.
#include "mod/playlayer_helpers.hpp"

#if GEODE_COMP_GD_VERSION >= 22090 && !defined(GDSOLVER_GEODE6)

#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/utils/Keyboard.hpp>

class $modify(KeybindDispatcher, cocos2d::CCKeyboardDispatcher) {
    GDSOLVER_HOOK_GUARD()
    bool dispatchKeyboardMSG(cocos2d::enumKeyCodes key, bool down, bool repeat, double timestamp) {
        using geode::KeyboardInputData;
        using geode::KeyboardModifier;
        // A key is the window's own callback delivering it, so none should arrive while another application has
        // the foreground. If one does, this says so (once per press, not per repeat); the hotkeys refuse it either
        // way (session_hotkeys.hpp, hotkeysFocused), and the game still gets it as it always did.
        if (down && !repeat && !hotkeysFocused())
            geode::log::info("hotkeys: key {} reached the dispatcher while another window has the foreground", (int)key);
        uint8_t mods = KeyboardModifier::None;
        if (this->getShiftKeyPressed()) mods |= KeyboardModifier::Shift;
        if (this->getControlKeyPressed()) mods |= KeyboardModifier::Control;
        if (this->getAltKeyPressed()) mods |= KeyboardModifier::Alt;
        if (this->getCommandKeyPressed()) mods |= KeyboardModifier::Super;
        KeyboardInputData data(key, down ? (repeat ? KeyboardInputData::Action::Repeat : KeyboardInputData::Action::Press)
                                         : KeyboardInputData::Action::Release,
                               KeyboardInputData::Native{0, 0}, timestamp, KeyboardModifier(mods));
        if (geode::KeyboardInputEvent(key).send(data) != geode::ListenerResult::Propagate) return true;
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
    }
};

#endif
