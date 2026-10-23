#pragma once
#include "core/PairPlacementMath.h"
#include <algorithm>
#include <numbers>

namespace AnimationInjector::PairAlignmentMath {
constexpr float pi = std::numbers::pi_v<float>;
inline float Wrap(float x) { return std::remainder(x, 2 * pi); }
struct Pose {
    float x = 0, y = 0, z = 0, yaw = 0;
};
inline bool Valid(Pose p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) && std::isfinite(p.yaw);
}
inline float Distance(Pose a, Pose b) { return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z); }
struct Plan {
    std::array<Pose, 2> goals;
    float cost = 0;
};
// Solve only after native Start accepts. Scope is yaw-only mark rotations;
// unsupported rotations keep native placement with the same visual blend.
// A single scene direction preserves the authored relative transform, while
// equal XY translation weights preserve the two starting actors' midpoint.
// Native leader height is retained; do not average uneven terrain heights.
inline std::optional<Plan> Solve(
    const std::array<Pose, 2>& start, const PairPlacementMath::Mark& leader, const PairPlacementMath::Mark& support) {
    if (!Valid(start[0]) || !Valid(start[1]) || !PairPlacementMath::Valid(leader) || !PairPlacementMath::Valid(support))
        return {};
    for (const auto* m : {&leader, &support})
        if (std::abs(m->rotation[0]) + std::abs(m->rotation[1]) > .001f) return {};
    const auto relative = PairPlacementMath::RelativePosition(leader, support);
    if (!relative) return {};
    const float localYaw = Wrap(2 * std::atan2(leader.rotation[2], leader.rotation[3]) -
                                2 * std::atan2(support.rotation[2], support.rotation[3]));
    const auto candidate = [&](float yaw) {
        const float s = std::sin(yaw), c = std::cos(yaw);
        const float x = (*relative)[0] * c + (*relative)[1] * s, y = -(*relative)[0] * s + (*relative)[1] * c;
        Pose b{(start[0].x + start[1].x - x) * .5f, (start[0].y + start[1].y - y) * .5f, start[1].z, Wrap(yaw)};
        Pose a{b.x + x, b.y + y, b.z + (*relative)[2], Wrap(yaw + localYaw)};
        float cost = 0;
        for (unsigned i = 0; i < 2; ++i) {
            const auto p = i ? b : a;
            const float d = Distance(p, start[i]), r = Wrap(p.yaw - start[i].yaw);
            cost += d * d + 2500 * r * r;
        }
        return Plan{{a, b}, cost};
    };
    // Seed the exact actor headings too: already aligned pairs need no motion.
    Plan best = candidate(start[1].yaw);
    const auto test = [&](float yaw) {
        const auto p = candidate(yaw);
        if (p.cost < best.cost) best = p;
    };
    test(start[0].yaw - localYaw);
    constexpr float step = 2 * pi / 720;
    for (unsigned i = 0; i < 720; ++i) test(-pi + i * step);
    float width = step;
    for (unsigned i = 0; i < 8; ++i) {
        const float y = best.goals[1].yaw;
        test(y - width * .5f);
        test(y + width * .5f);
        width *= .5f;
    }
    if (!std::isfinite(best.cost)) return {};
    for (unsigned i = 0; i < 2; ++i)
        if (!Valid(best.goals[i])) return {};
    return best;
}
// The same native mark constraint as Solve (start[0] is role 0, start[1] role 1). The anchor role (the
// victim: the scene's lead) keeps its position and only turns; the other role takes the whole move.
// The turn points the authored relation at the other role where it stands, so that role moves only
// along that line (authored distance minus current distance) instead of circling to the anchor's
// current facing. With no usable bearing (coincident roles, or a relation with no horizontal offset)
// the anchor also keeps its heading. `keepHeading` keeps it always (0.12.262: a player anchor, whose turn the
// engine does not take, so the other role takes the authored place around the player's own facing).
inline std::optional<Plan> Pivoted(const std::array<Pose, 2>& start, const PairPlacementMath::Mark& leader,
    const PairPlacementMath::Mark& support, unsigned anchor, bool keepHeading = false) {
    if (anchor > 1 || !Valid(start[anchor]) || !PairPlacementMath::Valid(leader) || !PairPlacementMath::Valid(support))
        return {};
    for (const auto* m : {&leader, &support})
        if (std::abs(m->rotation[0]) + std::abs(m->rotation[1]) > .001f) return {};
    const auto relative = PairPlacementMath::RelativePosition(leader, support);
    if (!relative) return {};
    const float localYaw = Wrap(2 * std::atan2(leader.rotation[2], leader.rotation[3]) -
                                2 * std::atan2(support.rotation[2], support.rotation[3]));
    // Role 0 = role 1 + R(role 1 yaw) * relative, and role 0 yaw = role 1 yaw + localYaw. A Skyrim
    // heading's bearing is atan2(x, y), and R(yaw) adds yaw to a local bearing, so matching the
    // bearing from role 1 to role 0 gives the same yaw1 whichever role is the anchor.
    float yaw1 = anchor == 1 ? start[1].yaw : Wrap(start[0].yaw - localYaw);
    const unsigned other = 1 - anchor;
    const float dx = start[0].x - start[1].x, dy = start[0].y - start[1].y;
    if (!keepHeading && Valid(start[other]) && std::hypot((*relative)[0], (*relative)[1]) >= 1 &&
        std::hypot(dx, dy) >= 1)
        yaw1 = Wrap(std::atan2(dx, dy) - std::atan2((*relative)[0], (*relative)[1]));
    const float s = std::sin(yaw1), c = std::cos(yaw1);
    const float x = (*relative)[0] * c + (*relative)[1] * s, y = -(*relative)[0] * s + (*relative)[1] * c;
    Plan plan;
    if (anchor == 1)
        plan.goals = {Pose{start[1].x + x, start[1].y + y, start[1].z + (*relative)[2], Wrap(yaw1 + localYaw)},
            Pose{start[1].x, start[1].y, start[1].z, yaw1}};
    else
        plan.goals = {Pose{start[0].x, start[0].y, start[0].z, Wrap(yaw1 + localYaw)},
            Pose{start[0].x - x, start[0].y - y, start[0].z - (*relative)[2], yaw1}};
    for (unsigned i = 0; i < 2; ++i)
        if (!Valid(plan.goals[i])) return {};
    if (Valid(start[other])) {
        const float d = Distance(plan.goals[other], start[other]), r = Wrap(plan.goals[other].yaw - start[other].yaw);
        plan.cost = d * d + 2500 * r * r;
    }
    return plan;
}
// The start blend's heading hold (0.12.279): a snapped role whose heading something else turned more than about
// a degree away from its goal is put back (TDM facing the crosshair while blocking turned a player attacker 26°).
inline constexpr float kHeadingSlack = .02f;
inline bool HeadingDrifted(float yawError) { return std::isfinite(yawError) && std::abs(yawError) > kHeadingSlack; }
// Wait for independent native frames, including orientation, before handoff.
struct Gate {
    float elapsed = 0;
    unsigned settled = 0;
    bool Step(float delta, bool samples, float positionError, float yawError) {
        if (!std::isfinite(delta) || delta <= 0) return false;
        elapsed += delta;
        const bool good = samples && std::isfinite(positionError) && positionError <= 2 && std::isfinite(yawError) &&
                          yawError <= .02f;
        settled = good ? settled + 1 : 0;
        return settled >= 2 && elapsed >= .05f;
    }
};
}
