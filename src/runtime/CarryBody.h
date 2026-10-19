#pragma once
#include "RE/B/BSFixedString.h"
#include "RE/B/BSPointerHandle.h"
#include "runtime/PairedRig.h"
#include <cstdint>
#include <string_view>

namespace AnimationInjector::CarryBody {
// Carry stage 3: the carried target as a physical body (CarryTargetMode::kPhysics).
//
// When the lift has attached the target, one ragdoll body (the <Carry part>, or the body nearest
// the carrier bone) stays keyframed to the carry attachment, and every other body simulates in
// world space: head, neck and legs hang, trail and swing with the carrier's walking, turning and
// stopping, and collide with the world. The lift's frozen last pose (kPose) stays the fallback: when
// physics cannot start or stops, the carry continues in that pose (Carry restarts the physics).
//
// Main thread only, driven by Carry. Wraps CarryPartialRagdoll::Session; the session keeps
// NiPointer/hkRefPtr ownership and revalidates the scene and world every frame.
enum class State : std::uint8_t {
    kOff,         // no physics (pose mode, not started, or ended)
    kSimulating,  // anchor keyframed, other bodies dynamic
    kReleasing,   // drop (cancel or putdown): waiting for the engine's knockdown to own the bodies
    kFaulted,     // safety stop; Carry ends physics and keeps (or drops) the target
};
// After a successful attach. Reads the ragdoll profile once per carry (runtime/RagdollProfile; a
// restart with the same generation reuses it): the lift
// clip's own (liftClip = the lift pair's clip path), else the rig's.
// kPose = physics unavailable (reason logged); stay in pose mode. kTargetSimulating = the target's own
// ragdoll still simulates its bodies: holding it in a pose would fight the engine (Carry lets it go).
enum class Start : std::uint8_t { kStarted, kPose, kTargetSimulating };
Start Begin(RE::ActorHandle carrier, RE::ActorHandle target, PairedRig::Kind rig, std::string_view liftClip,
    const RE::BSFixedString& part, const RE::BSFixedString& carrierBone, std::uint64_t generation);
// Every frame after the attachment pulse.
State Update(float delta);
// Drop (cancel or the putdown's CarryRelease): snapshot poses and velocities before the attachment
// is removed. After Carry
// requests the native knockdown, AdvanceRelease commits them once the engine owns every body.
bool BeginRelease();
enum class Release : std::uint8_t { kWaiting, kDone, kFailed };
Release AdvanceRelease();
// Idempotent. yield = the target died or went fully ragdoll: leave its bodies to the engine.
// Ignored while a cancel drop owns the bodies (kReleasing); AdvanceRelease finishes those.
void End(bool yield);
// Load / new game: end everything, including a pending release.
void Reset();
State Current();
}
