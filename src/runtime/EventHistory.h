#pragma once
#include "PCH.h"

// When each actor last received the animation events a recentEvent condition asks about (11-07, <Check
// kind="recentEvent" events="attackStop" within="0.5">): a sprint stop that comes right after a sprint attack's
// attackStop. Both streams count, events sent to the graph (NotifyAnimationGraph) and events the graph sends
// (annotations), the same two an event start hears. Only the names a loaded condition asks about are kept, as
// HitHistory does for hits; names match without regard to case, as the game's event names do.
namespace AnimationInjector::EventHistory {

// After the declarations load: the event names conditions ask about (none clears the history). Returns each name's
// id, in order; a name given twice gets one id.
std::vector<int> SetNames(const std::vector<std::string>& names);
bool Needed() noexcept;

// Main thread, every frame (from the event registry's update): game seconds, paused with the game.
void Advance(float delta);

// Any thread, from the notify hook and the animation event hook: `actor` received `event`. Does nothing for a name no
// condition asks about.
void Record(RE::Actor* actor, const char* event);

// The seconds since `actor` last received the event with `id`. False when it has not since the load. Any thread.
bool Ago(RE::ActorHandle actor, int id, float& ago);

// Load / new game: forgets every entry (the names stay).
void ResetOnLoad();

}  // namespace AnimationInjector::EventHistory
