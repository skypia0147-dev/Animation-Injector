#pragma once
#include "PCH.h"
#include "runtime/BodyPart.h"

// A part action's life (body="upper|rightArm|leftArm", docs/설계-상체레이어.md).
//
// Per actor, one action per layer (the upper body and each arm can run at once). Each layer has two slots
// (core/UpperLayer.h): an action that follows another on the same layer (a start while one plays, a transition such as
// a charge's start -> hold -> release) plays in the other slot and the layer's mix crosses to it over the new action's
// enter blend, the layer weight staying up. A request while the mix still crosses waits for it. Main thread.
//
//   start   through ActionRunner (a <Start> of any kind, its condition cards, an event QTE result): the runner hands a
//           part action here. Refused: first person, the layer not in the graph (UpperLayer::LayerReady), cooldown,
//           in the air, a full-body action of ours playing (Actions::BusyActive: one leaving does not count),
//           ragdoll / knocked / dead / sitting / mounted / swimming / in a killmove or pair.
//           Sprinting (user decision 11-20): the player's start stops the sprint and the sprint key held through it
//           is ignored until released (the gate below); an NPC's start is refused while it sprints.
//   play    the slot's clip index is set and the layer weight w eases 0 -> 1 over the enter blend (default 0.3 s); the
//           clip plays once (it blends out to end at its Exit time, default 0.05 s before the clip's end) or loops
//           (until maxHold, a transition or an interruption). <Do at> runs on the runner's clock.
//   links   its <Transition>s (runtime/ActionLinks.h) as a full-body action's: key, event and press kinds, on="end" at
//           the point it would start blending out, on="release" (maxHold counts as a release), end="true", condition
//           cards. A destination on the same layer crosses the mix; on another layer it starts there while this one
//           blends out; a full-body destination starts and this layer blends out over its enter blend. A full-body
//           action's transition to a part action exits the full body and starts the part (Link).
//   end     w eases 1 -> 0 over the exit blend (default 0.3 s); the action's cooldown starts as it begins to leave.
//   interrupted (exit blend): attack, block, weapon draw or sheathe, spell cast, in the air, sprint start, a
//           full-body action of ours, ragdoll / knocked / sitting / mounted / swimming / killmove / pair.
//           At once (w 0): death, the 3D unloaded, the player in first person.
//   load    Reset: every runner ends at once.
//
// Polled each frame rather than told by events: a state the actor is in ends the action whoever put it there.
// External state it touches and gives back (global rule 6-2): the layer variables (w 0 at every exit path, the slots'
// clip indices and the mix left). The sprint it stopped is not restored: the player sprints again with a new press.
namespace AnimationInjector::PartRunner {

// Main thread: whether `actor` may start part action `action` now (the start path's checks, ActionRunner::CanStart).
bool CanStart(RE::Actor* actor, int action);
// Main thread. false: refused (the reason in the debug log).
bool Start(RE::Actor* actor, int action, const char* origin);
// A transition to part action `target` (runtime/ActionLinks.h). `source`: the action it leaves, a part action of the
// actor (its layer, or another) or a full-body one. CanLink is the preflight: a full-body source counts as gone (the
// caller exits it before Link). Main thread.
bool CanLink(RE::Actor* actor, int source, int target);
bool Link(RE::Actor* actor, int source, int target);
// Part action `action` of `actor` blends out if it plays (an end="true" transition, a transition that did not go, a
// full-body destination taking over: `blend` its enter blend, below 0 the action's own exit blend). Main thread.
bool EndAction(RE::Actor* actor, int action, const char* why, float blend = -1.0f);
// Main thread, once a frame (the player's update): weights, clocks, interruptions, for every actor that has one.
void Update(float delta);
// Main thread: ends the layer's action of `actor` through its exit blend.
void End(RE::Actor* actor, int layer, const char* why);
// Main thread: every runner ends at once (loads).
void Reset();
// Whether `actor` has a part action running on `layer` (-1: any layer).
bool Running(RE::Actor* actor, int layer = -1);
// The sprint button hook (the gate above). Plugin load.
void Install();

}  // namespace AnimationInjector::PartRunner
