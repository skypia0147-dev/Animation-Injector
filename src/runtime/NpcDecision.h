#pragma once

// Combat starts (1.1.0, <Start combat="npc" every="0.5">): the runtime asks them for NPCs itself, so an NPC can open
// with a gap closer or another attack the vanilla AI would not choose. Every `every` seconds an NPC in high process
// that is in combat with a living, loaded target, has its weapon drawn, is not attacking (attack state none), stands
// (not knocked down, staggered or ragdolled) and plays none of our full-body actions asks each combat start in
// declaration order; the start is queued as an event start is (Actions::QueueStart) and its conditions read the combat
// target as the partner, so its distance, angle and chance checks decide.
//
// The vanilla AI keeps its own attacks: they reach the event starts and transitions as before.
//
// Each NPC's clocks are kept by FormID (no actor pointer is stored) and dropped once it leaves the high process list.
namespace AnimationInjector::NpcDecision {
// Main thread, once a frame (the player's update).
void Update(float delta);
// Loads and new games: every clock starts over.
void Reset();
}
