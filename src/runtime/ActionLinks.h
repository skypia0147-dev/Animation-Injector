#pragma once
#include "PCH.h"
#include "runtime/GameControls.h"
namespace AnimationInjector::ActionLinks {
void Begin(RE::ActorHandle actor, int action, bool paired, bool player, RE::ActorHandle partner = {});
void End(RE::ActorHandle actor, int action);
void ObserveEvent(RE::ActorHandle actor, const RE::BSFixedString& event);
void ObserveTime(RE::ActorHandle actor, int action, float time, float duration = 0.0f);
// Claim Connect keys for the whole source lifetime, including key-up events. `control` is the game control the
// press stands for (0.12.292): a transition on a control matches it from any device. `taken` = an open window took
// this press (a press on a game control is then kept from the game).
struct InputResult {
    bool claimed = false, taken = false;
};
InputResult Input(std::uint32_t code, bool gamepad, GameControl control = GameControl::kNone, bool down = true);
// An event a running action of `actor` has a transition on (10-29), as Input claims a key: its window open, not open
// yet or closed, or that transition's destination still waiting. An event start ignores it then: the transition
// takes the event (a closed window turns it into no new start either). Any thread.
bool ClaimsEvent(RE::ActorHandle actor, const RE::BSFixedString& event);
void Update(float delta);
// Charge (10-30). The runner of `action` reached its Exit point: an on="end" transition goes instead (true), else
// false and the runner exits as usual. Main thread.
bool ReachedEnd(RE::ActorHandle actor, int action);
// A loop's maxHold passed (an NPC's, or a player still holding): its key counts as released, so an on="release"
// transition goes (true); false when it has none, and the runner exits. Main thread.
bool ForceRelease(RE::ActorHandle actor, int action);
void ResetOnLoad();
// Main thread (ViewGuard): drops the actor's link windows and pending transitions, as a partner too.
void Cancel(RE::ActorHandle actor);
// An event QTE's result (0.12.264): the pair `actor` plays hands both over like a Connect with partnerTo, `actor` to
// `target` and its partner to `partnerTarget` (-1: none). False when `actor` plays no pair. Main thread.
bool RequestHandoff(RE::ActorHandle actor, int target, int partnerTarget);
// Keep inherited camera ownership while an actual destination is pending.
bool HasPendingTransition(RE::ActorHandle actor);
}
