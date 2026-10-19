#pragma once
#include <cmath>

namespace AnimationInjector {
// Retain the restoration lease through ragdoll/get-up and verify it after engine updates.
struct CameraRecovery {
    enum class Step { kWait, kApply, kComplete, kExpired };
    bool pending = false, applied = false, interrupted = false;
    float age = 0, readyTime = 0, stableTime = 0, retry = 0;
    unsigned attempts = 0;

    void Request() {
        if (!pending) {
            *this = {};
            pending = true;
        }
    }
    Step Update(float delta, bool blocked, bool restored) {
        if (!pending || !std::isfinite(delta) || delta < 0) return Step::kWait;
        age += delta;
        if (age >= 120.0f) return Step::kExpired;
        if (blocked) {
            interrupted = true;
            applied = false;
            readyTime = stableTime = retry = 0;
            return Step::kWait;
        }
        readyTime += delta;
        if (interrupted && readyTime < 0.25f) return Step::kWait;
        if (applied && restored) {
            stableTime += delta;
            return stableTime >= 0.5f ? Step::kComplete : Step::kWait;
        }
        stableTime = 0;
        retry -= delta;
        if (retry > 0) return Step::kWait;
        if (attempts >= 20) return Step::kExpired;
        ++attempts;
        applied = true;
        retry = 0.25f;
        return Step::kApply;
    }
};
}
