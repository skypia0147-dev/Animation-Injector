#pragma once
#include "PCH.h"

// Starts the action an event QTE's result names (0.12.249): at once when the actor is up, else the moment the
// engine hands a body it is getting up back to the character controller, so the action takes the place of the
// engine's get-up clip (steps: runtime/GetUpHandoffPlan.h). One waiting start at a time, the player's; main thread.
namespace AnimationInjector::GetUpHandoff {
// False when it cannot start or wait (absent, dead, held down, a paired action, or the engine's own get-up already
// past its first frames).
bool Start(RE::Actor* actor, int action);
void Update(float delta);
// The actor's natural get-up notifications (any thread), so they are never supplied twice.
void ObserveEvent(RE::Actor* actor, const RE::BSFixedString& tag);
void ResetOnLoad();
}
