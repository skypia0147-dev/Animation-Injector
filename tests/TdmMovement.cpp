#include "runtime/TdmMovement.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>

// A movement condition reads the player's direction as the character moves: forward in TDM's directional mode (10-30).
int main() {
    using AnimationInjector::TdmMovement::ForCharacter;
    using Mode = AnimationInjector::TdmMovement::Mode;
    constexpr int forward = 0, back = 1, left = 2, right = 3, none = 4, unknown = -1;
    // Directional mode: the player turns to the keys and runs forward; no key, no movement.
    for (int keys : {forward, back, left, right}) assert(ForCharacter(keys, Mode::kDirectional) == forward);
    assert(ForCharacter(none, Mode::kDirectional) == none);
    assert(ForCharacter(unknown, Mode::kDirectional) == unknown);
    // Without TDM, an old TDM, and its other modes: the keys, as always.
    for (Mode mode : {Mode::kAbsent, Mode::kDisabled, Mode::kVanillaStyle, Mode::kTargetLock})
        for (int keys : {forward, back, left, right, none, unknown}) assert(ForCharacter(keys, mode) == keys);
    // TDM's settings by its MCM ini (10-30): its API's mode cannot tell vanilla style from directional.
    using AnimationInjector::TdmMovement::FromSetting;
    using AnimationInjector::TdmMovement::IniInt;
    assert(FromSetting(2) == Mode::kDirectional && FromSetting(1) == Mode::kVanillaStyle &&
           FromSetting(0) == Mode::kDisabled);
    const char* ini = "\xEF\xBB\xBF[DirectionalMovement]\r\nuDirectionalMovementDrawn = 1\r\n; a note\r\n"
                      "uDirectionalMovementSheathed=2\r\n[Other]\r\nuDirectionalMovementDrawn = 0\r\n";
    assert(IniInt(ini, "DirectionalMovement", "uDirectionalMovementDrawn") == 1);
    assert(IniInt(ini, "DirectionalMovement", "uDirectionalMovementSheathed") == 2);
    assert(IniInt(ini, "Other", "uDirectionalMovementDrawn") == 0);
    assert(!IniInt(ini, "DirectionalMovement", "uDialogueMode"));
    assert(!IniInt(
        "[DirectionalMovement]\nuDirectionalMovementDrawn = x\n", "DirectionalMovement", "uDirectionalMovementDrawn"));
    std::cout << "TdmMovement ok\n";
    return 0;
}
