#pragma once
#include <algorithm>
#include <cmath>

namespace AnimationInjector::CarryLimbFrame {
// Completed poses belong to the attachment frame in which Havok solved them.
// Keep the same relative pose when transporting physics or displaying a sample.
template<class Transform>
Transform Rebase(const Transform& pose, const Transform& from, const Transform& to) {
    return to * (from.Invert() * pose);
}
// Angle (radians) of the turn from rotation `from` to `to`; 0 for non-finite input.
template<class Matrix>
float TurnAngle(const Matrix& from, const Matrix& to) {
    const auto d = to * from.Transpose();
    const float angle = std::acos(std::clamp((d.entry[0][0] + d.entry[1][1] + d.entry[2][2] - 1) * .5f, -1.0f, 1.0f));
    return std::isfinite(angle) ? angle : 0.0f;
}
// World-axis angular velocity (rad/s) that turns rotation `from` into `to` over dt, for matrices
// acting on column vectors (NiMatrix3 as read from Havok). Zero for invalid dt or a half turn.
template<class Point, class Matrix>
Point AngularVelocity(const Matrix& from, const Matrix& to, float dt) {
    if (!std::isfinite(dt) || dt <= 0) return {};
    const auto d = to * from.Transpose();
    const Point skew{d.entry[2][1] - d.entry[1][2], d.entry[0][2] - d.entry[2][0], d.entry[1][0] - d.entry[0][1]};
    const float cosine = std::clamp((d.entry[0][0] + d.entry[1][1] + d.entry[2][2] - 1) * .5f, -1.0f, 1.0f);
    const float angle = std::acos(cosine), sine = std::sin(angle);
    if (!std::isfinite(angle)) return {};
    if (angle < 1e-4f) return skew * (.5f / dt);  // skew = 2 sin(angle) axis
    if (sine < 1e-4f) return {};
    return skew * (angle / (2 * sine * dt));
}
}
