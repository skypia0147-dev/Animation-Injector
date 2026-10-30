#pragma once
#include "PCH.h"

// The last weapon hit each actor received (10-29), for the hitReceived condition (<Check kind="hitReceived">): a
// power attack, a bash, a sneak attack, blocked. From TESHitEvent (core/HitDispatch), weapon sources only: an
// enchanted weapon sends a second hit event for its enchantment, without the power attack flag, which would otherwise
// write over the weapon's. Kept only while a loaded declaration has such a check (SetNeeded), as ActionHistory.
namespace AnimationInjector::HitHistory {

struct Hit {
    bool power = false, bash = false, sneak = false, blocked = false;
};
// Whether `hit` is what an option names: normal (neither a power attack nor a bash), power, bash, sneak, blocked.
bool Matches(const Hit& hit, std::string_view option) noexcept;

// After the declarations load: whether any condition reads the history. False clears it.
void SetNeeded(bool needed);
bool Needed() noexcept;

// Main thread, every frame (from the event registry's update): game seconds, paused with the game.
void Advance(float delta);

// Main thread, from the hit event sink: `actor` was hit by a weapon. Does nothing when not Needed.
void Record(RE::ActorHandle actor, const Hit& hit);

// The last hit `actor` received and the seconds since. False when it received none since the load. Any thread.
bool LastOf(RE::ActorHandle actor, Hit& hit, float& ago);

// Load / new game: forgets every entry.
void ResetOnLoad();

}  // namespace AnimationInjector::HitHistory
