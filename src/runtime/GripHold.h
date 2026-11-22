#pragma once
#include "PCH.h"
#include "core/PoseTrackReader.h"
#include "runtime/GripSpec.h"

// gripHold / gripRelease (1.0.9): a hand's attachment bone (right: Weapon, left: Shield) held at the value it had
// when gripHold fired, past the clip that sent it, until gripRelease or an automatic release.
//
// Where: just under the hand bone switches, the point UpperLayer wraps (RootBehaviorGraph, and over it the part
// layers' blenders when there are part actions). Written at RootBehaviorGraph's output and again at each part layer's,
// so a part action moving the bone does not win over the hold (user decision: the hold wins), while the switches
// above (staff, crossbow, DW_ShieldAdjustment and Left Hand Equipment Overhaul over it) still decide their bones.
// The value is the bone's parent-relative transform there, so it follows the hand whatever the body does. Left-hand
// reverse grips show with Left Hand Equipment Overhaul 2.0's Adjustment_Additive (docs/설계-역수유지.md): the clip that
// takes the grip carries it; a left-hand hold moved to the other view sends that view's graph Adjustment_Additive
// itself, and Adjustment_Off when the hold ends.
//
// Views: the value is taken in the graph the actor shows (the active one) at its next generate. A view that is not the
// shown one gets that value moved by the reference-pose difference, in the parent frame:
// T = ref'.T + (v.T - ref.T), R = (v.R * ref.R^-1) * ref'.R.
//
// State per graph, actor FormID and hand; the graph address is an identity only (never dereferenced from the state).
// Main thread: Hold, Release, Update, ReleaseAll. Animation threads: Apply.
namespace AnimationInjector::GripHold {

void Hold(RE::Actor* a_actor, GripHand a_hand, GripView a_view);
// At once: the next frame shows the animation's value (no blend, user decision 2026-11-21).
void Release(RE::Actor* a_actor, GripHand a_hand, GripView a_view);
// Once a frame: takes what Apply captured and releases a hand whose actor
// sheathed, changed that hand's item, died, went ragdoll or unloaded.
void Update(float a_delta);
// Load and new game.
void ReleaseAll();

// Generate hooks call this with the output of a node just under the hand bone switches.
void Apply(const RE::hkbContext& a_context, PoseTrackReader::Output& a_output, bool a_layer = false);
// hkbStateMachine::Generate for RootBehaviorGraph (both views).
void Install();

}  // namespace AnimationInjector::GripHold
