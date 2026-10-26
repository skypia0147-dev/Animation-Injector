#pragma once
#include "PCH.h"

// The actions an actor finished last (0.12.296), for the previousAction condition (<Check kind="previousAction">).
// Kept only while a loaded declaration has such a check (SetNeeded): otherwise a finishing action records nothing
// and the clock does not run. One entry per actor; no per-frame work but the clock's addition.
namespace AnimationInjector::ActionHistory {

// After the declarations load: whether any condition reads the history. False clears it.
void SetNeeded(bool needed);
bool Needed() noexcept;

// Main thread, every frame (from the event registry's update): game seconds, paused with the game.
void Advance(float delta);

// Main thread, from ActionRunner::Finish: `actor` finished action `index`; `completed` = it played to its own exit
// (not cancelled, interrupted or ended by a transition). Does nothing when not Needed.
void Record(RE::ActorHandle actor, int index, bool completed);

// The last finished action of `actor`: its index, seconds since it ended and whether it completed. False when the
// actor finished none since the load.
struct Last {
    int index = -1;
    float ago = 0.0f;
    bool completed = false;
};
bool LastOf(RE::ActorHandle actor, Last& out);

// Load / new game: forgets every entry (their actors and times belong to the old game).
void ResetOnLoad();

}  // namespace AnimationInjector::ActionHistory
