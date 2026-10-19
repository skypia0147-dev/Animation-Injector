#pragma once
#include <algorithm>
#include <cmath>
#include <optional>

namespace AnimationInjector::CarryHoldMath {
// A bone takes the weight of the nearest configured bone on its path to the root,
// itself included. Unconfigured subtrees stay at 0 (untouched).
template<class Parent, class Configured>
float Inherited(int bone, Parent parent, Configured configured) {
    for (int depth = 0; bone >= 0 && depth < 256; ++depth, bone = parent(bone))
        if (const std::optional<float> weight = configured(bone)) return std::clamp(*weight, 0.0f, 1.0f);
    return 0;
}
// Parent-space blend from the animated local toward the hold local: rotation nlerp on
// the shortest arc, translation lerp, animated scale kept. The ends are exact.
template<class Transform, class Quaternion>
Transform Blend(const Transform& animated, const Transform& hold, float weight) {
    if (!(weight > 0)) return animated;
    Transform result = hold;
    result.scale = animated.scale;
    if (weight >= 1) return result;
    result.translate = animated.translate * (1 - weight) + hold.translate * weight;
    Quaternion a(animated.rotate), b(hold.rotate);
    b.Correct(a);
    Quaternion q(a.w * (1 - weight) + b.w * weight, a.x * (1 - weight) + b.x * weight,
        a.y * (1 - weight) + b.y * weight, a.z * (1 - weight) + b.z * weight);
    const float length = std::sqrt(q.Dot(q));
    if (!(length > 0.0001f)) return result;
    q.w /= length;
    q.x /= length;
    q.y /= length;
    q.z /= length;
    q.ToRotation(result.rotate);
    return result;
}
// The local a held bone is written with while the hold blends in (fade 0 to 1). With a start pose
// (the arm as drawn when the hold began, the lift's last playing sample) the hold is at the bone's
// full weight from the first frame and moves from start to the hold pose by itself: the animation
// under it is leaving the lift for locomotion over the same seconds (the lift's exit blend), and
// blending from it pulled the grip out and back (0.12.201 trace: about 25 degrees in 0.2 s, the
// carried body swinging with it). Without a start pose, from the animated local as before.
template<class Transform, class Quaternion>
Transform Held(
    const Transform& animated, const std::optional<Transform>& start, const Transform& hold, float weight, float fade) {
    fade = std::clamp(fade, 0.0f, 1.0f);
    if (!start) return Blend<Transform, Quaternion>(animated, hold, weight * fade);
    return Blend<Transform, Quaternion>(animated, Blend<Transform, Quaternion>(*start, hold, fade), weight);
}
}
