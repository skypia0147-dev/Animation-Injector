#pragma once
#include <algorithm>
#include <cmath>

// The run's share of a split direction blender (0 the walk clip .. 1 the run clip), without engine headers (tests).
// What it does and why: core/LocomotionDrive.h.
namespace AnimationInjector::Gait {

inline constexpr float kGaitSeconds = 0.2f;                // a stop's way down, for the whole 1 -> 0
inline constexpr float kStopRunShare = 0.25f;              // the share a stopping run keeps
inline constexpr float kWalkEase = 0.12f;                  // time constant of a run easing into a walk
inline constexpr float kRunEase = 0.06f;                   // time constant of a walk easing into a run
inline constexpr float kWalkFrom = 2.2f, kWalkTo = 1.08f;  // walking: Speed / walk speed over which the share falls
inline constexpr float kStopFrom = 1.8f;                   // running: Speed / walk speed below which a stop begins

// The share after `delta` seconds. `running`: the actor's running (or sprinting) state; `speed`: its Speed;
// `walkSpeed`: Actor::GetWalkSpeed(), the steady walking Speed.
inline float NextShare(float share, bool running, float speed, float walkSpeed, float delta) {
    share = std::clamp(share, 0.0f, 1.0f);
    const float step = delta / kGaitSeconds;
    // Standing: the share it stopped with stays through the move-stop cross-fade; one still above the stop's share
    // finishes its way down to it (a stop is over sooner than the share crosses).
    if (speed <= 0.0f) return share > kStopRunShare ? std::max(kStopRunShare, share - step) : share;
    const float ratio = walkSpeed > 1.0f ? speed / walkSpeed : (running ? kWalkFrom : 0.0f);
    float target = 0.0f;
    if (running) {
        target = std::clamp((ratio - 1.0f) / (kStopFrom - 1.0f), kStopRunShare, 1.0f);
        if (target < share) return std::max(target, share - step);  // a stop: linear
    } else
        target = std::clamp((ratio - kWalkTo) / (kWalkFrom - kWalkTo), 0.0f, 1.0f);
    const float next = share + (target - share) * (1.0f - std::exp(-delta / (target > share ? kRunEase : kWalkEase)));
    return std::fabs(next - target) < 0.002f ? target : next;
}

}  // namespace AnimationInjector::Gait
