#pragma once
#include "RE/B/BSPointerHandle.h"
#include <cstdint>

namespace RE {
class hkbClipGenerator;
}
namespace AnimationInjector {
struct CarryHoldSpec;
}

namespace AnimationInjector::CarryHold {
// Carry stage 2: the carrier's upper-body hold pose.
//
// While a carry is held, selected carrier bones take a fixed hold pose on top of the
// ordinary animation, per bone weight: 1 = hold pose, 0 = untouched, between = blend in
// parent space (rotation nlerp, translation lerp, animated scale kept). A weight applies
// to the named bone and every descendant until another named bone overrides it. Only
// bones the carrier's animation graph drives are written, so physics-driven nodes (SMP,
// equipment) under a held arm are never overwritten.
//
// "lift-end" is the lift clip's last frame, sampled from the playing clip, so a lift cut
// short by movement recovery still holds the pose the clip ends in (blending in over
// blendIn). The last playing sample is only a fallback when the clip cannot be sampled.
//
// Ownership: Carry drives the lifecycle on the main thread. The carrier is the player
// (Carry only starts from the player); no actor pointer or handle is kept. Bone owners are
// NiPointers revalidated against the player's current 3D, and the pose buffer holds copied
// values only. Handles are never resolved under the mutex the animation step also takes.
// The write runs at the end of the player's UpdateAnimation (measured in 0.12.167: the engine
// writes the arm locals there, after the player update, and computes world transforms from them
// before the next frame, also while a menu pauses the game).
enum class Result : std::uint8_t {
    kReady,            // resolved; holding, or waiting for the pose
    kDisabled,         // no <Hold>, or no bone with a positive weight
    kNoGraph,          // carrier graph or its bone list is unavailable
    kUnknownBone,      // a configured name is not driven by the carrier's graph
    kPoseUnavailable,  // no pose yet
};

// Installs the per-frame write (kDataLoaded).
void Install();
// Resolve configured bones against the carrier's active graph (lift pair start).
// Nothing is written yet. A failure only disables the hold; the carry continues.
Result Begin(RE::ActorHandle carrier, const CarryHoldSpec& spec);
// The carrier's lift clip, from the synchronized clip hook on the thread evaluating it:
// samples the clip's last frame once.
void SampleClip(const RE::hkbClipGenerator& clip);
// Called with every end-pose sample while the lift plays. Keeps the last playing locals as the
// fallback pose and as the start the hold blends in from.
Result CaptureLiftEnd();
// Start writing (end pose frozen, or the carry re-attached after a transfer). The clip's
// last frame blends in over blendIn; a fallback sample starts at full weight because the
// arm is already there. Idempotent while holding.
void Hold();
// Blend back to the animation over blendOut (cancel drop, suspend), over the given seconds
// (putdown: the putdown Action's entry blend, so the arm goes straight into the putdown pose
// instead of back to locomotion first), or stop at once. The write itself also stops at once for
// a ragdolled, dead or re-3D'd carrier.
void Release(bool immediate, float seconds = -1.0f);
// Load / new game: forget everything without touching the skeleton.
void Reset();
}
