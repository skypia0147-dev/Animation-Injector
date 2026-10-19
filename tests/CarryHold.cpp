#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <RE/N/NiQuaternion.h>
#include <RE/N/NiTransform.h>
#include "runtime/CarryHoldMath.h"

int main() {
    using namespace AnimationInjector::CarryHoldMath;
    unsigned checks = 0;
    const auto check = [&](bool ok) {
        ++checks;
        if (!ok) {
            std::cerr << "Carry hold check failed: " << checks << '\n';
            std::exit(1);
        }
    };
    // 0 root, 1 spine2, 2 R clavicle, 3 R upper arm, 4 R forearm, 5 R hand, 6 L clavicle,
    // 7 L upper arm, 8 R finger.
    constexpr std::array<int, 9> parents{-1, 0, 1, 2, 3, 4, 1, 6, 5};
    const auto weights = [&](const std::map<int, float>& config) {
        std::array<float, 9> out{};
        for (int i = 0; i < 9; ++i)
            out[i] = Inherited(
                i, [&](int b) { return parents[b]; },
                [&](int b) -> std::optional<float> {
                    const auto it = config.find(b);
                    return it == config.end() ? std::nullopt : std::optional<float>(it->second);
                });
        return out;
    };
    // Right clavicle: the whole right arm and its fingers, nothing else.
    auto w = weights({{2, 1.0f}});
    check(w[2] == 1 && w[3] == 1 && w[4] == 1 && w[5] == 1 && w[8] == 1);
    check(w[0] == 0 && w[1] == 0 && w[6] == 0 && w[7] == 0);
    // A lower bone's own value wins for its subtree.
    w = weights({{2, 0.5f}, {3, 1.0f}});
    check(w[2] == 0.5f && w[3] == 1 && w[5] == 1 && w[8] == 1);
    w = weights({{2, 1.0f}, {5, 0.0f}});
    check(w[3] == 1 && w[4] == 1 && w[5] == 0 && w[8] == 0);
    // Out-of-range authoring is clamped; a malformed cycle terminates.
    check(weights({{2, 1.5f}})[4] == 1 && weights({{2, -1.0f}})[4] == 0);
    check(Inherited(3, [](int b) { return b; }, [](int) { return std::optional<float>{}; }) == 0);

    RE::NiTransform animated, hold;
    animated.rotate.SetEulerAnglesXYZ(0.3f, -0.2f, 0.9f);
    animated.translate = {10, 2, -3};
    animated.scale = 1.1f;
    hold.rotate.SetEulerAnglesXYZ(-0.4f, 0.5f, -1.2f);
    hold.translate = {14, -2, 1};
    hold.scale = 0.9f;
    const auto angle = [](const RE::NiMatrix3& a, const RE::NiMatrix3& b) {
        float trace = 0;
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < 3; ++k) trace += a.entry[k][i] * b.entry[k][i];
        return std::acos(std::clamp((trace - 1) / 2, -1.0f, 1.0f));
    };
    const auto blend = [](const RE::NiTransform& a, const RE::NiTransform& b, float f) {
        return Blend<RE::NiTransform, RE::NiQuaternion>(a, b, f);
    };
    // Exact ends: weight 0 is the animation untouched, weight 1 the hold pose with animated scale.
    auto r = blend(animated, hold, 0);
    check(std::memcmp(&r, &animated, sizeof(r)) == 0);
    r = blend(animated, hold, 1);
    check(std::memcmp(&r.rotate, &hold.rotate, sizeof(r.rotate)) == 0 && r.translate == hold.translate &&
          r.scale == animated.scale);
    // Halfway: equal arcs to both ends, midpoint translation, orthonormal rotation.
    r = blend(animated, hold, 0.5f);
    const float total = angle(animated.rotate, hold.rotate);
    check(std::abs(angle(animated.rotate, r.rotate) - total / 2) < 0.01f);
    check(std::abs(angle(r.rotate, hold.rotate) - total / 2) < 0.01f);
    check((r.translate - RE::NiPoint3{12, 0, -1}).Length() < 1e-4f && r.scale == animated.scale);
    const auto& m = r.rotate.entry;
    const float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                      m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                      m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    check(std::abs(det - 1) < 1e-3f);
    // Shortest arc: a hold equal to the animation stays put at every weight.
    for (float f : {0.25f, 0.5f, 0.75f}) check(angle(blend(animated, animated, f).rotate, animated.rotate) < 1e-3f);
    // Shortest arc across the quaternion sign flip: 179 deg and -179 deg are 2 deg apart.
    RE::NiTransform left = animated, right = animated;
    left.rotate.SetEulerAnglesXYZ(0, 0, 3.12414f);
    right.rotate.SetEulerAnglesXYZ(0, 0, -3.12414f);
    r = blend(left, right, 0.5f);
    check(angle(left.rotate, r.rotate) < 0.02f && angle(r.rotate, right.rotate) < 0.02f);
    // Held: the hold blends in over the same seconds the lift's exit blend takes the animation from
    // the lift pose (animated here) to locomotion (hold here stands for the far pose).
    const auto held = [](const RE::NiTransform& a, const std::optional<RE::NiTransform>& s, const RE::NiTransform& h,
                          float w, float f) { return Held<RE::NiTransform, RE::NiQuaternion>(a, s, h, w, f); };
    const RE::NiTransform& lift = animated;
    const RE::NiTransform& locomotion = hold;
    float oldWorst = 0, newWorst = 0;
    for (int k = 0; k <= 20; ++k) {
        const float t = k / 20.0f;
        const auto live = blend(lift, locomotion, t);  // the animation under the hold, leaving the lift
        oldWorst = std::max(oldWorst, angle(held(live, std::nullopt, lift, 1, t).rotate, lift.rotate));
        const auto kept = held(live, lift, lift, 1, t);
        newWorst = std::max(newWorst, angle(kept.rotate, lift.rotate) + (kept.translate - lift.translate).Length());
        check(kept.scale == live.scale);
    }
    // Before: the arm left the lift pose by about a quarter of the way to locomotion and came back.
    check(oldWorst > 0.2f * total && newWorst < 1e-3f);
    // A start away from the hold pose (a lift cut short) moves to it by the fade, whatever runs below.
    for (float f : {0.25f, 0.5f, 0.75f}) {
        const auto moving = held(locomotion, lift, left, 1, f);
        check(angle(moving.rotate, blend(lift, left, f).rotate) < 1e-3f);
    }
    // Ends and partial weights: fade 1 is the hold pose; a half-weight bone keeps half the animation.
    check(angle(held(locomotion, lift, left, 1, 1).rotate, left.rotate) < 1e-4f);
    const auto half = held(locomotion, lift, lift, 0.5f, 0.3f);
    check(angle(half.rotate, blend(locomotion, lift, 0.5f).rotate) < 1e-3f);
    // Without a start pose nothing changes from the plain blend.
    const auto plain = held(locomotion, std::nullopt, lift, 0.8f, 0.5f);
    const auto before = blend(locomotion, lift, 0.4f);
    check(std::memcmp(&plain, &before, sizeof(plain)) == 0);
    std::cout << checks << " carry hold checks passed\n";
}
