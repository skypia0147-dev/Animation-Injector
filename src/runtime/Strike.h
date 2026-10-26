#pragma once
#include "PCH.h"
#include "runtime/StrikeSpec.h"

// The strike executor (StrikeSpec.h). EventEffects runs it for EventOp::kStrike, as it runs every event function.
namespace AnimationInjector::Strike {

// Main thread. `attacker` strikes now as `spec` says: picks its targets and hits each once. `partner` is its pair
// partner (StrikeSpec::Pick::kPartner), empty outside a pair. A spec with a duration opens a window that Update
// keeps running; the actors it already hit are not hit again in it. Does nothing when the attacker is not
// loaded, dead or ragdolled.
void Run(RE::Actor* attacker, RE::ActorHandle partner, const StrikeSpec& spec);

// Main thread, every frame (from EventEffects::Update): runs the open windows, hitting actors as they come in,
// and closes those whose time is up or whose attacker is gone (not loaded, dead or ragdolled).
void Update(float delta);

// Load / new game: closes every window (their actors belong to the old game).
void ResetOnLoad();

// Main thread: true while a strike sends its own TESHitEvent (StrikeSpec::hitEvent). HitDispatch then leaves the
// pair and the carry alone: a strike's hit on a pair partner or a carried actor is the action's own, not a third
// party's hit that ends them.
bool Sending();

}  // namespace AnimationInjector::Strike
