#pragma once
#include "PCH.h"
#include "runtime/GameControls.h"
namespace AnimationInjector::GameControls {
// The stub game's user events (PCH.h UserEvents) as controls.
inline GameControl Of(const std::string& name) {
    return name == "Jump"           ? GameControl::kJump
           : name == "Sprint"       ? GameControl::kSprint
           : name == "Ready Weapon" ? GameControl::kReadyWeapon
                                    : GameControl::kNone;
}
}
