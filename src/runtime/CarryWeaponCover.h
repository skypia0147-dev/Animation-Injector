#pragma once
#include "RE/B/BSPointerHandle.h"
#include "runtime/CarrySpec.h"

namespace RE {
class Actor;
}

namespace AnimationInjector::CarryWeaponCover {
// What a drawn weapon means to a carry (CarrySpec::weapon). Main thread only, driven by Carry.
//   sheathed (default): a carry starts only sheathed; drawing a weapon or spell drops the target.
//   sheathe: as the lift starts the carrier's weapon goes to its sheath with no animation, then as
//     sheathed. Until then (and if that fails) its weapons are hidden (WeaponCull) and an attack or a
//     spell cast drops the target. The target's weapons stay hidden until it is let go.

// Drawing a weapon or readying a spell leaves kSheathed at once (kWantToDraw), before the draw
// animation plays. Carrying is not a combat state.
bool WeaponOut(RE::Actor* actor);
// The lift starts (Carry::PairStarted).
void Begin(CarryWeapon mode, RE::ActorHandle carrier, RE::ActorHandle target);
// Every lifting frame. weapon="sheathe" puts the carrier's weapon away once it plays the lift
// (`liftPlaying`), then keeps the graph's default state sheathed.
void Lifting(RE::Actor* carrier, bool liftPlaying);
// This frame's weapon rule: nullptr keeps carrying, otherwise the cancel reason.
const char* Rule(RE::Actor* carrier);
// The carry ended: every hidden weapon shows.
void End(const char* reason);
// Every frame: covered nodes stay culled (a newly equipped weapon gets a new node).
void Maintain();
// Load / new game: show everything and forget the carry.
void Reset(const char* reason);
}
