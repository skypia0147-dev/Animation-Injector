#pragma once
#include <array>
#include <cmath>
#include <optional>

namespace AnimationInjector::PairPlacementMath {
using Vector = std::array<float, 3>;
using Quaternion = std::array<float, 4>;
struct Mark {
    Vector translation{};
    Quaternion rotation{0, 0, 0, 1};
};
inline bool Valid(const Mark& mark) {
    for (float x : mark.translation)
        if (!std::isfinite(x)) return false;
    float norm = 0;
    for (float x : mark.rotation) {
        if (!std::isfinite(x)) return false;
        norm += x * x;
    }
    return std::abs(norm - 1.0f) <= 0.002f;
}
inline Vector Rotate(const Quaternion& q, const Vector& p) {
    const Vector cross{q[1] * p[2] - q[2] * p[1], q[2] * p[0] - q[0] * p[2], q[0] * p[1] - q[1] * p[0]};
    const float dot = q[0] * p[0] + q[1] * p[1] + q[2] * p[2];
    Vector out{};
    for (int i = 0; i < 3; ++i) out[i] = 2 * ((q[3] * q[3] - 0.5f) * p[i] + dot * q[i] + q[3] * cross[i]);
    return out;
}
// Native Start composes the support mark with the inverse leader mark.
// Values are native mark units; do not introduce a Havok world-scale conversion.
inline std::optional<Vector> RelativePosition(const Mark& leader, const Mark& support) {
    if (!Valid(leader) || !Valid(support)) return std::nullopt;
    auto inverse = leader.rotation;
    for (int i = 0; i < 3; ++i) inverse[i] = -inverse[i];
    auto origin = Rotate(inverse, leader.translation);
    for (float& x : origin) x = -x;
    auto out = Rotate(support.rotation, origin);
    for (int i = 0; i < 3; ++i) {
        out[i] += support.translation[i];
        if (!std::isfinite(out[i])) return std::nullopt;
    }
    return out;
}
}
