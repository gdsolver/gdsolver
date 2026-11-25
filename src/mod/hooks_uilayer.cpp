// UILayer hooks: the practice-mode checkpoint keys and buttons, for the human takeover
// (mod/takeover.hpp).
#include "mod/playlayer_helpers.hpp"
#include <Geode/modify/UILayer.hpp>
#ifdef GDSOLVER_GEODE6
#include <Geode/utils/Keyboard.hpp>
#endif

class $modify(UILayer) {
    GDSOLVER_HOOK_GUARD()
    // cfg rngearly (config.hpp): PlayLayer::resetLevel calls this straight after it stores the trigger seed; pin the stream here, before the attempt's start triggers fire.
    void toggleMenuVisibility(bool visible) {
        rngFixEarly();
        UILayer::toggleMenuVisibility(visible);
    }

#ifndef GDSOLVER_GEODE6
    // X while a person plays a takeover: the takeover's newest strict checkpoint goes, never the
    // start point's (takeover::removeByKey).
    //
    // Caught here, at the key, because the key does not go through onDeleteCheck: handleKeypress
    // (0x4cd830) jumps straight to PlayLayer::removeCheckpoint (0x3b7f00) for X, where Z goes to
    // onCheck. And not at removeCheckpoint, which the game also calls itself with the same
    // argument: from checkCollisions (0x214746) and in a loop from removeAllCheckpoints (0x3b8065)
    // that runs until the game's list is empty. Once per press: the key's repeat is not a second
    // removal.
    //
    // On 2.209 this function (0x6713c0) is UILayer::handleAction, and what arrives in `key` is the
    // action id the key is bound to (10001 + the jump-table index; delete-checkpoint is the case at
    // 0x6715bb, index 7, id 10008), not the key code: the test on KEY_X alone never matched there,
    // the game's own removal ran (which does nothing once a restore has emptied its list), and the
    // strict checkpoints piled up. No key code reaches 10000, so both spellings are tested.
    //
    // Not built against Geode 6 (see the key listener below): this function belongs to the
    // CCKeyboardDelegate base of UILayer, whose pointer the game passes (UILayer + 0x158: the
    // function reads the game layer at this+0x100, which the bindings put at UILayer+0x258). Geode 6's
    // detour is handed the UILayer pointer and the call to the original does not give the base's
    // back, so with this hook installed every key that reaches the function crashed (Space: a read of
    // the game layer through the wrong object) or did nothing (Escape never paused) -- measured on
    // 2.2091 with Geode 6.0.0, 2026-11-25.
    bool handleAction(int action, bool down, double timestamp) {
        constexpr int kActionDeleteCheckpoint = 10008;
        if ((action == (int)cocos2d::enumKeyCodes::KEY_X || action == kActionDeleteCheckpoint)
            && takeover::personDriving()) {
            static bool s_down = false;
            if (down && !s_down) takeover::removeByKey(PlayLayer::get());
            s_down = down;
            return true;
        }
        return UILayer::handleAction(action, down, timestamp);
    }
#endif

    // ...and the on-screen delete button, which is onDeleteCheck's (0x4ce0e0).
    void onDeleteCheck(cocos2d::CCObject* sender) {
        if (takeover::personDriving()) {
            takeover::removeByKey(PlayLayer::get());
            return;
        }
        UILayer::onDeleteCheck(sender);
    }
};

#ifdef GDSOLVER_GEODE6
// X while a person plays a takeover, on Geode 6: the loader posts a KeyboardInputEvent from its window
// procedure before the game sees the key, and a listener that returns Stop keeps the key from the game --
// so the game's own removal (which the takeover's checkpoints are not in) never runs, and no hook on a
// function of the game's keyboard delegate is needed. Once per press: a repeat or the release is taken
// and does nothing. Only a takeover (takeover::personDriving), so a person playing with the mod merely
// loaded keeps X as the game has it.
$execute {
    if (!gdsolver::exeguard::allowed()) return;   // not this game build: nothing of the mod's runs (exe_guard.hpp)
    geode::KeyboardInputEvent().listen([](geode::KeyboardInputData& data) {
        if (data.key != cocos2d::enumKeyCodes::KEY_X || !takeover::personDriving())
            return geode::ListenerResult::Propagate;
        if (data.action == geode::KeyboardInputData::Action::Press) takeover::removeByKey(PlayLayer::get());
        return geode::ListenerResult::Stop;
    }).leak();
}
#endif
