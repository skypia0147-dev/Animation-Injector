#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <RE/N/NiTransform.h>
#include "core/CarryLimbFrame.h"

int main() {
    using namespace AnimationInjector::CarryLimbFrame;
    unsigned checks = 0;
    const auto check = [&](bool ok) {
        ++checks;
        if (!ok) {
            std::cerr << "Limb frame check failed: " << checks << '\n';
            std::exit(1);
        }
    };
    const auto transform = [](float x, float yaw) {
        RE::NiTransform t;
        t.translate = {x, 97000, -13800};
        t.rotate.SetEulerAnglesXYZ(0, 0, yaw);
        return t;
    };
    RE::NiTransform local;
    local.translate = {20, 5, 45};
    local.rotate.SetEulerAnglesXYZ(.2f, .3f, .4f);
    for (float scale : {.8f, 1.f, 1.2f})
        for (float turn : {-3.f, -1.5f, 0.f, 1.5f, 3.f}) {
            auto old = transform(-65000, 0);
            old.scale = scale;
            auto current = transform(-64900, turn);
            current.scale = scale;
            const auto body = old * local;
            // Old code interpreted a completed old-world sample under today's root.
            const auto wrong = current.Invert() * body;
            check((wrong.translate - local.translate).Length() > 35);
            const auto moved = Rebase(body, old, current);
            const auto result = current.Invert() * moved;
            check((result.translate - local.translate).Length() < .05f);
            for (unsigned row = 0; row < 3; ++row)
                for (unsigned col = 0; col < 3; ++col)
                    check(std::abs(result.rotate.entry[row][col] - local.rotate.entry[row][col]) < .0001f);
            // Multiple render updates can reuse one physics sample without either
            // double-transporting it or drifting back toward its old world position.
            for (unsigned frame = 0; frame < 120; ++frame) {
                auto next = transform(-64900 + float(frame), turn + frame * .005f);
                next.scale = scale;
                const auto fromOriginal = Rebase(body, old, next);
                const auto fromTransported = Rebase(moved, current, next);
                check((fromOriginal.translate - fromTransported.translate).Length() < .05f);
                check((next.Invert() * fromOriginal).translate.GetDistance(local.translate) < .05f);
            }
        }
    // Angular velocity: rotate `from` by a known axis-angle (Rodrigues) and recover it.
    const auto rodrigues = [](RE::NiPoint3 axis, float angle) {
        axis = axis / axis.Length();
        const float s = std::sin(angle), c = 1 - std::cos(angle);
        const float k[3][3]{{0, -axis.z, axis.y}, {axis.z, 0, -axis.x}, {-axis.y, axis.x, 0}};
        RE::NiMatrix3 r;
        for (unsigned row = 0; row < 3; ++row)
            for (unsigned col = 0; col < 3; ++col) {
                float k2 = 0;
                for (unsigned m = 0; m < 3; ++m) k2 += k[row][m] * k[m][col];
                r.entry[row][col] = (row == col ? 1.f : 0.f) + s * k[row][col] + c * k2;
            }
        return r;
    };
    RE::NiMatrix3 from;
    from.SetEulerAnglesXYZ(.2f, -.7f, 1.1f);
    for (const RE::NiPoint3 axis : {RE::NiPoint3{0, 0, 1}, RE::NiPoint3{1, 0, 0}, RE::NiPoint3{.3f, -.8f, .5f}})
        for (float angle : {0.f, .00005f, .05f, .8f, 2.5f})
            for (float dt : {1.f / 30, 1.f / 144}) {
                const auto omega = AngularVelocity<RE::NiPoint3>(from, rodrigues(axis, angle) * from, dt);
                const auto expected = axis / axis.Length() * (angle / dt);
                check((omega - expected).Length() <= .001f * (1 + expected.Length()));
            }
    // A counterclockwise turn about +Z (column vectors) is +Z angular velocity.
    RE::NiMatrix3 yaw;
    yaw.entry[0][0] = std::cos(.1f), yaw.entry[0][1] = -std::sin(.1f);
    yaw.entry[1][0] = std::sin(.1f), yaw.entry[1][1] = std::cos(.1f);
    check(std::abs(AngularVelocity<RE::NiPoint3>(RE::NiMatrix3{}, yaw, .01f).z - 10) < .001f);
    // TurnAngle: the angle of the same turns, and 0 for no turn.
    for (const RE::NiPoint3 axis : {RE::NiPoint3{0, 0, 1}, RE::NiPoint3{.3f, -.8f, .5f}})
        for (float angle : {0.f, .1f, .52f, 1.f, 3.f})
            check(std::abs(TurnAngle(from, rodrigues(axis, angle) * from) - angle) < .002f);
    check(AngularVelocity<RE::NiPoint3>(from, from, 0).Length() == 0);
    check(AngularVelocity<RE::NiPoint3>(from, from, std::numeric_limits<float>::quiet_NaN()).Length() == 0);
    std::cout << checks << " carried limb frame checks passed\n";
}
