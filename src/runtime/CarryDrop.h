#pragma once
#include <algorithm>
#include <cmath>
namespace AnimationInjector::CarryDrop {
// A carry cancelled while its target is held drops the target as a ragdoll (Carry::AdvanceDrop).
enum class HandoffResult { kWaiting, kComplete, kFailed };
struct NativeHandoff {
    bool active = false, requested = false;
    float elapsed = 0;
    void Start() {
        active = true;
        requested = false;
        elapsed = 0;
    }
    // A native knockdown is issued only after collision/controller restoration,
    // and at most once. A timeout releases bookkeeping, never forces get-up.
    template<class RestoreController, class Request, class Commit>
    HandoffResult Advance(float delta, RestoreController restore, Request request, Commit commit) {
        if (!active) return HandoffResult::kFailed;
        if (std::isfinite(delta) && delta > 0) elapsed += std::min(delta, 0.1f);
        if (elapsed >= 2.0f) {
            active = false;
            return HandoffResult::kFailed;
        }
        if (!requested && restore()) {
            requested = true;
            if (!request()) {
                active = false;
                return HandoffResult::kFailed;
            }
        }
        if (requested) {
            const auto result = commit();
            if (result != HandoffResult::kWaiting) {
                active = false;
                return result;
            }
        }
        return HandoffResult::kWaiting;
    }
};
}
