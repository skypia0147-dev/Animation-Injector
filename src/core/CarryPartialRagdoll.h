#pragma once

#include "RE/B/BSFixedString.h"
#include "RE/B/BSPointerHandle.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace AnimationInjector::CarryPartialRagdoll {

// One carried actor's native ragdoll under carry control (carry stage 3, runtime/CarryBody).
// One owned ragdoll body stays keyframed to its node as the carry anchor and every other body
// simulates in world space. Any skeleton with a ragdoll driver works (vanilla chicken: Pelvis,
// Neck0-2, HEAD, both ankles).
//
// World space: keyframed bodies follow their targets with the matching velocity, and the simulated
// bodies trail, swing on turns and speed changes, and keep their momentum when released. A per-frame
// jump over 60 units or a turn over 30 degrees (a teleport or a snap turn) is carried rigidly for
// that frame, and so is a held body's keyframe jump past KeyedJumpLimit.
enum class Phase : std::uint8_t { kIdle, kReady, kSimulating, kFaulted, kAwaitNativeRelease };
enum class Result : std::uint8_t {
    kReady,
    kBusy,
    kInvalidActor,
    kSceneUnavailable,
    kWorldMismatch,
    kUnsupportedSkeleton,
    kForeignPhysicsOwner,
    kUnsafeUpdatePath,
    kActivationFailed
};
enum class EndReason : std::uint8_t { kFullRagdoll, kSceneChange, kLoadGame, kActorRemoved, kFault };
// Simulated-body tuning while carried; restored before the bodies return to the engine. Prepare
// clamps each to its range.
struct Dynamics {
    float linearDamping = 0.8f;   // at least this: drag, a light body trails like a flag
    float angularDamping = 3.0f;  // at least this
    float gravity = 1.0f;         // gravity factor, at least this [1, 4]
    float inertia = 1.0f;         // rotational inertia multiplier, mass unchanged [1, 10]
    float follow = 0.0f;          // share [0, 1] of the attachment's velocity change given to the
                                  // simulated bodies directly, not through the constraints
    // Cap on each ragdoll joint's friction torque [0, 100]; negative keeps the native value. A
    // vanilla chicken's 10 holds its light feet and lower neck joints still.
    float jointFriction = -1.0f;
};
// Per-body values from a rig profile (runtime/RagdollProfile). Unset members keep Dynamics (body)
// or the native joint. Names are matched once at Prepare, case-insensitively.
struct BodyTuning {
    std::string bone;      // node carrying an owned ragdoll body
    bool simulate = true;  // false: keyframed to its node, like the anchor
    std::optional<float> linearDamping, angularDamping, gravity, inertia;
    // The ragdoll joint to the nearest body above. Friction replaces the Dynamics cap; angles are
    // radians (cone: maximum; plane and twist: minimum, maximum).
    std::optional<float> friction, cone;
    std::optional<std::pair<float, float>> plane, twist;
    // A bodiless bone between this body and the body above (a chicken's thigh above its foot):
    // the joint pivot moves onto it, and it turns with this body's swing.
    std::string swingFrom;
};
struct Request {
    RE::ActorHandle carrier;
    RE::ActorHandle target;
    std::uint64_t carryGeneration = 0;
    // The node whose ragdoll body stays fixed. It must carry an owned ragdoll body of this actor, or
    // Prepare fails.
    RE::BSFixedString anchor;
    Dynamics dynamics;
    std::vector<BodyTuning> bodies;
    // [Debug] bRagdollTrace: record every physics step for HKX_Edit's calibration replay
    // (core/RagdollTrace). The rig and profile only label the file.
    bool trace = false;
    std::string traceRig, traceProfile;
};
struct Status {
    Phase phase = Phase::kIdle;
    Result result = Result::kSceneUnavailable;
    std::uint32_t selectedBodies = 0;
    std::uint32_t anchoredBodies = 0;
    float maximumJointError = 0.0f;
    RE::BSFixedString maximumJointBone;  // where maximumJointError was measured
    std::uint64_t physicsSteps = 0;
    // Times a joint pulled past the safety limit and the bodies were put back to their last sound
    // shape (relative to the anchor) instead of a safety stop. More than three within a second, a
    // non-finite sample or an attachment discontinuity still stop (kFaulted).
    std::uint32_t repairs = 0;
};

class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    // Read-only inspection first: resolve live handles, scene, world, body maps,
    // constraints and ownership. Failure must not change any engine state.
    Result Prepare(const Request& request);

    // Transactional activation. Roll back every acquired engine setting on failure.
    // Keep the actor's knock/ragdoll gameplay state unchanged.
    Result Enable();

    // Invoke only from verified physics-safe phases, never an arbitrary render tick.
    // Keyframe the held bodies before simulation; place the simulated bodies' nodes afterward,
    // relative to the anchor's keyframe now (never yesterday's world origin).
    void BeforePhysics(float gameDelta);
    void AfterPhysics(float gameDelta);

    // Freeze the current carried pose for a bounded native knockdown handoff.
    // Caller detaches transport, restores its controller lease and requests the
    // engine knockdown only after this succeeds. No standing-pose restoration.
    Result BeginNativeRelease();
    // kBusy waits for actual native ragdoll + dynamic bodies. kReady commits
    // saved poses/velocities exactly once and yields the registered bodies.
    Result AdvanceNativeRelease();

    // Idempotent cleanup for unload/reset/fault. Never insert an old scene into
    // a new world. Restore only state still owned by this exact session generation.
    void Invalidate(EndReason reason);

    [[nodiscard]] Status ReadStatus() const;

private:
    // This owns our bookkeeping, not engine objects. Actor handles and engine
    // smart pointers retain engine resources; no long-lived raw Actor pointers.
    struct State;
    std::unique_ptr<State> state_;
};

}
