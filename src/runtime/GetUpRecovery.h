#pragma once
#include <cmath>

namespace AnimationInjector::GetUpRecovery {
// A ragdoll function (0.12.278) ragdolls a role while that role's graph is still inside the
// paired state; the graph reaches FullyRagdoll about 0.2 s later. The engine requests the
// get-up once, when the ragdoll comes to rest (knock state kQueued): it sends GetUpBegin, and
// only FullyRagdoll consumes it (TurnOnMatchingRagdoll arms the 0.5 s GetUpTimerMod, whose
// GetUpStart moves the knock state to kGetUp). A role pushed down while it already lies on the
// ground rests within 0.07 s, so the request reaches the paired state and is lost, and the
// actor stays in kQueued indefinitely. A watch replays the request only after the native path
// has clearly stalled: first GetUpBegin alone, then RagdollInstant followed by GetUpBegin.
enum class Step { kNone, kRequestGetUp, kEnterRagdoll, kRecovered, kGone, kGiveUp };

// RE::KNOCK_STATE_ENUM values.
inline constexpr int kKnockNormal = 0;
inline constexpr int kKnockQueued = 5;
inline constexpr int kKnockGetUp = 6;
// The native kQueued -> kGetUp step takes 0.51 s (GetUpTimerMod alarm 0.5 s).
inline constexpr float kStallSeconds = 1.0f;
// RagdollInstant activates FullyRagdoll; its modifiers must be active before GetUpBegin.
inline constexpr float kSettleSeconds = 0.1f;
inline constexpr int kMaxAttempts = 3;
inline constexpr float kWatchSeconds = 20.0f;

struct Watch {
    float life = 0.0f;
    float queued = 0.0f;
    float settle = -1.0f;
    int attempts = 0;
};

struct Sample {
    bool present = false;  // Handle resolves and 3D is loaded.
    bool dead = false;
    bool held = false;  // Paralysis or bleedout keeps kQueued on purpose.
    int knock = kKnockNormal;
};

inline Step Advance(Watch& watch, const Sample& sample, float delta) {
    if (!sample.present || sample.dead) return Step::kGone;
    if (sample.knock == kKnockGetUp || sample.knock == kKnockNormal) return Step::kRecovered;
    if (!std::isfinite(delta) || delta < 0.0f) delta = 0.0f;
    watch.life += delta;
    if (watch.life >= kWatchSeconds) return Step::kGiveUp;
    const bool stalled = sample.knock == kKnockQueued && !sample.held;
    if (watch.settle >= 0.0f) {
        watch.settle -= delta;
        if (watch.settle > 0.0f) return Step::kNone;
        watch.settle = -1.0f;
        return stalled ? Step::kRequestGetUp : Step::kNone;
    }
    watch.queued = stalled ? watch.queued + delta : 0.0f;
    if (watch.queued < kStallSeconds) return Step::kNone;
    if (watch.attempts >= kMaxAttempts) return Step::kGiveUp;
    watch.queued = 0.0f;
    // Assume only the request was lost first; afterwards restore the ragdoll state as well.
    if (watch.attempts++ == 0) return Step::kRequestGetUp;
    watch.settle = kSettleSeconds;
    return Step::kEnterRagdoll;
}
}
