#pragma once
#include <cstdint>

// The input side of LocomotionSplit::Toggle: a keyboard press (DirectInput code) outside menus. Switches the <Locomotion>
// ladders when it is [Debug] iLadderToggleKey. Main thread. Kept apart from LocomotionSplit.h so the input sink needs
// no graph types.
namespace AnimationInjector::LocomotionToggle {
void OnKeyDown(std::uint32_t code);
}
