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

Quat Multiply(const Quat& a, const Quat& b) { return Normalize(Mul(a, b)); }

Quat AimTurn(const Quat& clipSpine, const Quat& spineModel, const Quat& spineRest, float yaw, float pitch) {
    const Quat rest = Normalize(spineRest);
    const Quat clipHeading = TwistZ(Mul(Normalize(clipSpine), Conjugate(rest)));
    const Quat nowHeading = TwistZ(Mul(Normalize(spineModel), Conjugate(rest)));
    const Quat aimYaw{0.0f, 0.0f, std::sin(yaw * 0.5f), std::cos(yaw * 0.5f)};
    // Heading: from the spine's to the clip's own, turned by the camera's.
    const Quat heading = Mul(aimYaw, Mul(clipHeading, Conjugate(nowHeading)));
    // Pitch about the aim's side axis: the model's +X turned by the camera's heading.
    const Quat raise{std::sin(pitch * 0.5f), 0.0f, 0.0f, std::cos(pitch * 0.5f)};
    const Quat pitchTurn = Mul(aimYaw, Mul(raise, Conjugate(aimYaw)));
    return Normalize(Mul(pitchTurn, heading));
}

Quat TurnLocal(const Quat& parentModel, const Quat& d, const Quat& local) {
    const Quat p = Normalize(parentModel);
    return Normalize(Mul(Conjugate(p), Mul(Normalize(d), Mul(p, Normalize(local)))));
}

std::array<Quat, 3> SpreadWaist(const Quat& parentModel, const Quat& pelvisLocal, const std::array<Quat, 3>& locals,
    const std::array<Quat, 4>& rests, float amount) {
    if (!(amount > 0.0f)) return locals;
    const auto heading = [&](const Quat& model, int bone) {
        const Quat t = TwistZ(Mul(Normalize(model), Conjugate(Normalize(rests[bone]))));
        return 2.0f * std::atan2(t[2], t[3]);
    };
    const auto wrap = [](float a) { return std::remainder(a, 6.2831853f); };
    const Quat p = Normalize(parentModel);
    const Quat m0 = Mul(p, Normalize(locals[0])), m1 = Mul(m0, Normalize(locals[1])),
               m2 = Mul(m1, Normalize(locals[2]));
    const float h[4] = {heading(Mul(p, Normalize(pelvisLocal)), 0), heading(m0, 1), heading(m1, 2), heading(m2, 3)};
    const float joint[3] = {wrap(h[1] - h[0]), wrap(h[2] - h[1]), wrap(h[3] - h[2])};
    const float share = (joint[0] + joint[1] + joint[2]) / 3.0f;
    std::array<Quat, 3> out = locals;
    Quat parent = p;
    for (int i = 0; i < 3; ++i) {
        const float c = (share - joint[i]) * std::min(amount, 1.0f);
        const Quat turn{0.0f, 0.0f, std::sin(c * 0.5f), std::cos(c * 0.5f)};
        out[i] = TurnLocal(parent, turn, locals[i]);
        parent = Mul(parent, out[i]);
    }
    return out;
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
