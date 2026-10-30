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
    std::cout << "TdmMovement ok\n";
    return 0;
}
