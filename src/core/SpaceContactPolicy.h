#pragma once
#include "runtime/ConditionFlow.h"
#include <cmath>

namespace AnimationInjector::SpaceContactPolicy {
enum class Surface { kParticipant, kNonPhysical, kFixed, kUnknown };
// Provisional static-volume policy; ground support and full motion envelopes are separate checks.
struct Summary {
    unsigned ignored = 0, support = 0, blocked = 0, unknown = 0;
    void Add(Surface kind, float normalZ, float contactHeight, float stepHeight = 20.0f) {
        if (kind == Surface::kParticipant || kind == Surface::kNonPhysical) {
            ++ignored;
            return;
        }
        if (kind != Surface::kFixed || !std::isfinite(normalZ) || std::abs(normalZ) > 1.001f ||
            !std::isfinite(contactHeight) || !std::isfinite(stepHeight) || stepHeight < 0) {
            ++unknown;
            return;
        }
        if (normalZ >= 0.70710678f && contactHeight <= stepHeight)
            ++support;
        else
            ++blocked;
    }
    ConditionFlow::Value Result(bool queryComplete) const {
        if (!queryComplete || unknown) return ConditionFlow::Value::kUnavailable;
        return blocked ? ConditionFlow::Value::kFalse : ConditionFlow::Value::kTrue;
    }
};
}
