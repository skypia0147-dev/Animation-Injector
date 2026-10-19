#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <RE/N/NiMatrix3.h>
#include <RE/N/NiPoint3.h>

namespace AnimationInjector::CarryJointAngles {
// The angles a Havok joint's limits bound, from the two bodies' world rotations and the joint's
// local frames (the rotations of hkpSetLocalTransformsConstraintAtom transformA / transformB, as
// matrices acting on column vectors). A ragdoll joint's columns are twist, plane and their cross;
// a limited hinge's are axis, perp1 and perp2. The measures are HKX_Edit's
// (hkxviewer/ragdoll_sim.py RagdollSim._angles): its replays of game traces follow Havok, and a
// carried start pose past a limit moves back exactly to it in the game's first step.

// Radians, in the order of the limits they meet: coneLimit (maximum), planesLimit, twistLimit.
struct Ragdoll {
    float cone = 0.0f, plane = 0.0f, twist = 0.0f;
};

namespace detail {
inline RE::NiPoint3 Column(const RE::NiMatrix3& m, unsigned c) { return {m.entry[0][c], m.entry[1][c], m.entry[2][c]}; }
inline RE::NiPoint3 Unit(const RE::NiPoint3& v) {
    const float length = v.Length();
    return length > 1e-12f ? v / length : v;
}
// `v` with its part along the unit `axis` removed, normalized.
inline RE::NiPoint3 Across(const RE::NiPoint3& v, const RE::NiPoint3& axis) { return Unit(v - axis * v.Dot(axis)); }
inline float Clamp(float v) { return std::clamp(v, -1.0f, 1.0f); }
}

// bodyA / bodyB: the bodies' world rotations; frameA / frameB: the joint's local frames.
inline Ragdoll RagdollAngles(
    const RE::NiMatrix3& bodyA, const RE::NiMatrix3& frameA, const RE::NiMatrix3& bodyB, const RE::NiMatrix3& frameB) {
    using namespace detail;
    const auto twistA = Unit(bodyA * Column(frameA, 0)), planeA = Unit(bodyA * Column(frameA, 1));
    const auto twistB = Unit(bodyB * Column(frameB, 0)), planeB = Unit(bodyB * Column(frameB, 1));
    Ragdoll r;
    r.cone = std::acos(Clamp(twistA.Dot(twistB)));
    r.plane = std::asin(Clamp(twistA.Dot(planeB)));
    // Twist: the plane axes turned apart about the mean twist axis.
    const auto mean = Unit(twistA + twistB);
    const auto u = Across(planeA, mean), v = Across(planeB, mean);
    r.twist = std::atan2(mean.Dot(u.Cross(v)), u.Dot(v));
    return r;
}
// Limited hinge: B's perp1 turned from A's about A's axis, measured on A (the convention under
// which vanilla rest poses sit inside their limits, a knee's -110..0 degrees).
inline float HingeAngle(
    const RE::NiMatrix3& bodyA, const RE::NiMatrix3& frameA, const RE::NiMatrix3& bodyB, const RE::NiMatrix3& frameB) {
    using namespace detail;
    const auto axis = Unit(bodyA * Column(frameA, 0)), perpA = Unit(bodyA * Column(frameA, 1));
    const auto perpB = Across(Unit(bodyB * Column(frameB, 1)), axis);
    return -std::atan2(axis.Dot(perpA.Cross(perpB)), perpA.Dot(perpB));
}

// Limits widened so the start angle sits inside with `pad` radians to spare (HKX_Edit's
// widen_to_start uses 1 degree). A limit already clear of the start by `pad` is unchanged, and
// so is every limit when the start is not finite.
// Ragdoll limits in CarryPartialRagdoll's Limits() order: cone maximum, plane minimum, plane
// maximum, twist minimum, twist maximum.
inline std::array<float, 5> Widen(std::array<float, 5> limits, const Ragdoll& start, float pad) {
    if (!std::isfinite(start.cone) || !std::isfinite(start.plane) || !std::isfinite(start.twist)) return limits;
    limits[0] = std::max(limits[0], start.cone + pad);
    limits[1] = std::min(limits[1], start.plane - pad);
    limits[2] = std::max(limits[2], start.plane + pad);
    limits[3] = std::min(limits[3], start.twist - pad);
    limits[4] = std::max(limits[4], start.twist + pad);
    return limits;
}
// Hinge limits: minimum, maximum.
inline std::pair<float, float> Widen(std::pair<float, float> limits, float start, float pad) {
    if (!std::isfinite(start)) return limits;
    return {std::min(limits.first, start - pad), std::max(limits.second, start + pad)};
}
}
