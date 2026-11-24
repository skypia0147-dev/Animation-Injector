#pragma once
#include "PCH.h"
#include "runtime/SignalNames.h"
#include <string>
#include <vector>

// OAR signals (1.0.9, user design 11-24): `<Action signal="Charge">` keeps the bool graph variable
// bAI_<AnimationSet id>_Charge true on an actor while an action carrying that signal plays on it (full-body or part,
// through transitions between actions of the same signal), and false otherwise. An Open Animation Replacer submod swaps
// the locomotion files on it (condition CompareValues, graphVariable "bAI_<set>_<name>" of type Bool == 1, with
// "interruptible": true and a blend time), so a charge or an aim walks with its own animations at no graph cost beyond
// one bool per signal. Blocking and the other vanilla states need no signal: OAR has conditions for them.
//
// The variables go into every third-person character master graph and its project's variable lookup
// (core/GraphInjector, core/EventMaps), as the shared full-body entry's do, so Actor::SetGraphVariableBool and OAR's
// GetGraphVariableBool find them by name. NPCs get them too.
//
// Written by polling, not at each exit: an actor touched by a start is checked every frame until none of its actions
// carries a signal and every variable written true was written false again (all exit paths, global rule 6-2). A write
// the active graph refuses (the player in first person) is tried again each frame. A load starts every graph over with
// them false.
//
// Plays = a full-body runner in State::kActive (not leaving), a part layer coming in or playing (not blending out).
namespace AnimationInjector::Signals {

// The distinct signal variables, in declaration order: what the character master graphs get. Built from the
// declarations on first use (any thread).
const std::vector<std::string>& Variables();
// Index into Variables() of action `action`'s signal, or -1.
int SignalOf(int action);

// Main thread: an action of `actor` that carries a signal started or took over (ActionRunner, PartRunner). It is
// checked from the next Update.
void Touch(RE::Actor* actor);
// Main thread, once a frame (the player's update): for every touched actor, the signals of the actions it plays,
// written where they changed. An actor with none left and nothing to undo, or gone, is dropped.
void Update();
// Loads: the touched actors are forgotten; the new graphs hold every signal false.
void Reset();

}  // namespace AnimationInjector::Signals
