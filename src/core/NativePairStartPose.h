#pragma once
#include "runtime/PairAlignmentMath.h"
#include "RE/H/hkQsTransform.h"

namespace AnimationInjector::NativePairStartPose {
// Native Activate copies character.worldFromModel into two generator caches.
// Keep the engine scale, but use the same world placement as native Start.
inline std::optional<RE::hkQsTransform> Rebase(const RE::hkQsTransform& cached, PairAlignmentMath::Pose goal) {
    if (!PairAlignmentMath::Valid(goal)) return std::nullopt;
    PairPlacementMath::Mark before;
    for (unsigned i = 0; i < 3; ++i) {
        before.translation[i] = cached.translation.quad.m128_f32[i];
        const float scale = cached.scale.quad.m128_f32[i];
        if (!std::isfinite(scale) || scale < .01f || scale > 100) return std::nullopt;
    }
    for (unsigned i = 0; i < 4; ++i) before.rotation[i] = cached.rotation.vec.quad.m128_f32[i];
    if (!PairPlacementMath::Valid(before)) return std::nullopt;
    auto result = cached;
    result.translation = {goal.x, goal.y, goal.z, 0};
    // Actor yaw is clockwise around Z, opposite quaternion positive Z.
    result.rotation.vec = {0, 0, -std::sin(goal.yaw * .5f), std::cos(goal.yaw * .5f)};
    return result;
}
}
