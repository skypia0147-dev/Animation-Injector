#include "core/UpperStabilize.h"

#include <cmath>

namespace AnimationInjector::UpperStabilize {
namespace {
Quat Mul(const Quat& a, const Quat& b) {
    // Hamilton product, (x, y, z, w): the rotation b first, then a.
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0], a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}
Quat Conjugate(const Quat& q) { return {-q[0], -q[1], -q[2], q[3]}; }
Quat Normalize(const Quat& q) {
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(n > 1e-8f)) return {0.0f, 0.0f, 0.0f, 1.0f};
    return {q[0] / n, q[1] / n, q[2] / n, q[3] / n};
}
}  // namespace

Quat ChainRotation(std::span<const Quat> chain) {
    Quat out{0.0f, 0.0f, 0.0f, 1.0f};
    for (const auto& local : chain) out = Normalize(Mul(out, local));
    return out;
}

Quat TwistZ(const Quat& q) {
    const float n = std::sqrt(q[2] * q[2] + q[3] * q[3]);
    if (!(n > 1e-6f)) return {0.0f, 0.0f, 0.0f, 1.0f};
    return {0.0f, 0.0f, q[2] / n, q[3] / n};
}

Quat SpineLocal(const Quat& parent, const Quat& rest, const Quat& clipParent, const Quat& clipSpine,
    const Quat& outputLocal, float w) {
    const Quat p = Normalize(parent), r = Normalize(rest), cp = Normalize(clipParent);
    // Turns about the up axis away from rest: the output's (the legs' heading) and the clip's own (its stance).
    const Quat outputTurn = TwistZ(Mul(p, Conjugate(r)));
    const Quat clipTurn = TwistZ(Mul(cp, Conjugate(r)));
    // The clip's spine relative to its parent, on that parent turned but upright.
    const Quat spineInClip = Mul(Conjugate(cp), Normalize(clipSpine));
    const Quat model = Mul(outputTurn, Mul(clipTurn, Mul(r, spineInClip)));
    return Slerp(outputLocal, Normalize(Mul(Conjugate(p), model)), w);
}

Quat Slerp(const Quat& a, const Quat& target, float w) {
    const Quat from = Normalize(a);
    if (!(w > 0.0f)) return from;
    if (w >= 1.0f) return Normalize(target);
    // Slerp on the shortest arc.
    const Quat t = Normalize(target);
    float dot = from[0] * t[0] + from[1] * t[1] + from[2] * t[2] + from[3] * t[3];
    Quat to = t;
    if (dot < 0.0f) {
        dot = -dot;
        to = {-t[0], -t[1], -t[2], -t[3]};
    }
    if (dot > 0.9995f) {
        Quat lerp{};
        for (int k = 0; k < 4; ++k) lerp[k] = from[k] + (to[k] - from[k]) * w;
        return Normalize(lerp);
    }
    const float theta = std::acos(dot), sinTheta = std::sin(theta);
    const float x = std::sin((1.0f - w) * theta) / sinTheta, y = std::sin(w * theta) / sinTheta;
    return Normalize(
        {x * from[0] + y * to[0], x * from[1] + y * to[1], x * from[2] + y * to[2], x * from[3] + y * to[3]});
}

}  // namespace AnimationInjector::UpperStabilize
