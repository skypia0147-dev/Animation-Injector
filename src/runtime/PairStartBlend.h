#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <RE/N/NiQuaternion.h>
#include <RE/N/NiTransform.h>
#include <algorithm>
#include <cmath>

namespace AnimationInjector::PairStartBlend {
// The visible root between the pose held before a start and the native motion (weight 0 to 1).
inline RE::NiTransform Blend(const RE::NiTransform& held, const RE::NiTransform& motion, float weight) {
    if (weight <= 0) return held;
    if (weight >= 1) return motion;
    RE::NiTransform result;
    result.translate = held.translate + (motion.translate - held.translate) * weight;
    result.scale = held.scale + (motion.scale - held.scale) * weight;
    RE::NiQuaternion from(held.rotate), to(motion.rotate);
    to.Correct(from);
    RE::NiQuaternion q{from.w + (to.w - from.w) * weight, from.x + (to.x - from.x) * weight,
        from.y + (to.y - from.y) * weight, from.z + (to.z - from.z) * weight};
    const float norm = std::sqrt(q.Dot(q));
    q.w /= norm;
    q.x /= norm;
    q.y /= norm;
    q.z /= norm;
    q.ToRotation(result.rotate);
    return result;
}
// Undo only our last exact write; a subsequent engine/physics pose wins.
template<class Transform>
struct Layer {
    Transform original{}, applied{};
    bool owned = false;
    template<class Equal>
    bool Restore(Transform& current, Equal equal) {
        const bool restore = owned && equal(current, applied);
        if (restore) current = original;
        owned = false;
        return restore;
    }
    void Apply(Transform& current, const Transform& desired) {
        original = current;
        applied = desired;
        current = desired;
        owned = true;
    }
};
// Both roles consume the same actual clip clock. Pending time never consumes blend.
struct Clock {
    float duration = 0, latest = 0;
    bool began = false;
    void Begin(float seconds) {
        *this = {};
        duration = std::isfinite(seconds) ? std::clamp(seconds, 0.f, 5.f) : 0;
    }
    float Weight(bool playing, float self, float target) {
        if (!playing || !std::isfinite(self) || !std::isfinite(target) || self < 0 || target < 0)
            return began ? Value() : 0;
        began = true;
        latest = std::max(latest, std::min(self, target));
        return Value();
    }
    float Value() const {
        const float t = duration > 0 ? std::clamp(latest / duration, 0.f, 1.f) : 1.f;
        return t * t * (3 - 2 * t);
    }
};
// The engine owns the model/marks. This affects only the visible child root.
inline RE::NiTransform Local(
    const RE::NiTransform& parent, const RE::NiTransform& captured, const RE::NiTransform& motion, float weight) {
    return parent.Invert() * Blend(captured, motion, weight);
}
}
