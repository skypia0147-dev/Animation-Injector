#pragma once
#include "PCH.h"
namespace AnimationInjector::ActionLinks {
void Begin(RE::ActorHandle actor, int action, bool paired, bool player, RE::ActorHandle partner = {});
void End(RE::ActorHandle actor, int action);
void ObserveEvent(RE::ActorHandle actor, const RE::BSFixedString& event);
void ObserveTime(RE::ActorHandle actor, int action, float time, float duration = 0.0f);
// Claim Connect keys for the whole source lifetime, including key-up events.
bool Input(std::uint32_t code, bool gamepad, bool down = true);
void Update(float delta);
void ResetOnLoad();
// Main thread (ViewGuard): drops the actor's link windows and pending transitions, as a partner too.
void Cancel(RE::ActorHandle actor);
// An event QTE's result (0.12.264): the pair `actor` plays hands both over like a Connect with partnerTo, `actor` to
// `target` and its partner to `partnerTarget` (-1: none). False when `actor` plays no pair. Main thread.
bool RequestHandoff(RE::ActorHandle actor, int target, int partnerTarget);
// Keep inherited camera ownership while an actual destination is pending.
bool HasPendingTransition(RE::ActorHandle actor);
}
