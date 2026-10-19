#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace AnimationInjector::RagdollTrace {
// Carry-physics recorder for calibrating HKX_Edit's ragdoll preview against the game
// (docs/설계-래그돌-프로필과-미리보기.md, chapter 4 / P5). On with [Debug] bRagdollTrace.
// HKX_Edit replays a trace: same rig, values and start state, the anchor driven along the
// recorded attachment frames with the recorded step times, and the game's bodies drawn over
// its own. The Havok post-simulation callback appends into storage reserved at Begin, so
// recording never allocates or touches the disk inside a physics step; Write runs on the main
// thread when the carry physics ends.

// A rigid frame in game units, world space. Rotation is a unit quaternion (w, x, y, z).
struct Pose {
    std::array<float, 3> position{};
    std::array<float, 4> rotation{1.0f, 0.0f, 0.0f, 0.0f};
};
struct BodyState {
    Pose pose;                       // the rigid body's frame (not its node's)
    std::array<float, 3> linear{};   // game units / s
    std::array<float, 3> angular{};  // rad / s, world axes
};
// What Havok actually holds for one body at Begin: after the rig profile, the actor scale and
// Havok's half-precision storage. The preview must reproduce these, not the profile's numbers.
struct BodyInfo {
    std::string bone;
    bool simulated = false;               // false: keyframed (the anchor, or simulate="false")
    float mass = 0.0f;                    // kg
    std::array<float, 3> inertia{};       // principal inertia, kg * game units^2 (body axes)
    std::array<float, 3> centerOfMass{};  // body frame, game units
    float linearDamping = 0.0f;
    float angularDamping = 0.0f;
    float gravityFactor = 0.0f;
    float timeFactor = 0.0f;
    // Havok's hkUFloat8 codes: an index into an engine table, not exposed by CommonLibSSE.
    std::uint8_t maxLinearVelocityCode = 0;
    std::uint8_t maxAngularVelocityCode = 0;
};
// One ragdoll joint as the game runs it (after the profile's limits and friction).
struct JointInfo {
    std::string bodyA, bodyB;
    float friction = 0.0f;  // maximum friction torque, kg * game units^2 / s^2
    float cone = 0.0f;      // radians
    std::array<float, 2> plane{}, twist{};
};
// One joint limit the carry widened so the start pose sits inside it (CarryJointAngles::Widen):
// ragdoll "cone", "planeMin", "planeMax", "twistMin", "twistMax"; limited hinge "hingeMin",
// "hingeMax". Radians. A replay applies these over the profile's limits.
struct WidenedLimit {
    std::string bodyA, bodyB;
    std::string limit;
    float from = 0.0f, to = 0.0f;
};
struct Header {
    std::string rig;      // PairedRig name, e.g. "chicken"
    std::string profile;  // profile file, empty = vanilla values
    std::string anchor;   // the keyframed anchor bone
    float actorScale = 1.0f;
    float follow = 0.0f;             // Dynamics::follow as applied
    std::array<float, 3> gravity{};  // the hkpWorld's gravity, game units / s^2
    float solverTau = 0.0f;
    float solverDamping = 0.0f;
    std::int32_t solverSteps = 0;       // hkpSolverInfo numSteps
    std::int32_t solverMicroSteps = 0;  // hkpSolverInfo numMicroSteps
    std::vector<BodyInfo> bodies;       // the order of BodyState in every step
    std::vector<JointInfo> joints;
    std::vector<WidenedLimit> widened;  // already in `joints` for ragdoll joints
};

class Recorder {
public:
    // Main thread. Reserves `capacity` steps; later steps are dropped and counted.
    void Begin(Header header, std::size_t capacity);
    // Havok post-simulation callback, under the session's sample lock. `delta` is the completed
    // step's deltaTime, `attachment` the carry attachment frame the anchor followed during it,
    // `bodies` in Header::bodies order. No allocation, no I/O; ignored when not active.
    void Add(float delta, const Pose& attachment, std::span<const BodyState> bodies) noexcept;
    // Main thread. JSON Lines: line 1 = the header object, then one array per step:
    // [time, delta, attachment(7), then per body: position(3) rotation(4) linear(3) angular(3)].
    // Returns false when the file can't be written. The recording stays until Clear/Begin.
    bool Write(const std::filesystem::path& file) const;
    void Clear() noexcept;

    bool Active() const noexcept { return active_; }
    std::size_t Steps() const noexcept { return steps_.size(); }
    std::size_t Dropped() const noexcept { return dropped_; }

private:
    struct Step {
        float time = 0.0f;  // seconds since Begin (sum of step deltas)
        float delta = 0.0f;
        Pose attachment;
    };
    Header header_;
    std::vector<Step> steps_;
    std::vector<BodyState> states_;  // Steps() * header_.bodies.size(), step-major
    std::size_t capacity_ = 0;
    std::size_t dropped_ = 0;
    float time_ = 0.0f;
    bool active_ = false;
};

// <SKSE log folder>/AnimationInjector/RagdollTrace/<rig>-<YYYYMMDD-HHMMSS>.jsonl. Creates the
// folder and removes the oldest traces beyond `keep`. Main thread.
std::filesystem::path NextFile(std::string_view rig, std::size_t keep = 10);
}
