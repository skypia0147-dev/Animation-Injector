#pragma once
#include "PCH.h"
#include "runtime/GameControls.h"

// The game side of game controls (runtime only: the game's UserEvents and ControlMap).
namespace AnimationInjector::GameControls {
// The control a button event stands for in gameplay (its user event), kNone when it is none of ours.
GameControl Of(const RE::BSFixedString& userEvent);
// The keys the player has bound to a control now: a keyboard scan code, a mouse button (+256) and a gamepad
// button, each 0 when unbound.
struct Bound {
    std::uint32_t key = 0, mouse = 0, gamepad = 0;
};
Bound BoundKeys(GameControl control);
}  // namespace AnimationInjector::GameControls
