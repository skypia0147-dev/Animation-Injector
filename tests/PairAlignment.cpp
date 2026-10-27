#include "runtime/PairAlignmentMath.h"
#include "core/NativePairRoles.h"
#include <iostream>
#include <cstdlib>
#include <limits>
using namespace AnimationInjector::PairAlignmentMath;
namespace {
namespace PairPlacementMath = AnimationInjector::PairPlacementMath;
inline Pose Interpolate(Pose a, Pose b, float t) {
    t = std::clamp(t, 0.f, 1.f);
    t = t * t * (3 - 2 * t);
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, Wrap(a.yaw + Wrap(b.yaw - a.yaw) * t)};
}
// Test oracle: the native mark constraint with the initiator body held at its current pose.
inline std::optional<Pose> TargetFor(
    Pose self, const PairPlacementMath::Mark& leader, const PairPlacementMath::Mark& support) {
    if (!Valid(self) || !PairPlacementMath::Valid(leader) || !PairPlacementMath::Valid(support)) return {};
    for (const auto* m : {&leader, &support})
        if (std::abs(m->rotation[0]) + std::abs(m->rotation[1]) > .001f) return {};
    const auto relative = PairPlacementMath::RelativePosition(leader, support);
    if (!relative || std::abs((*relative)[2]) > .01f) return {};
    const float turn = Wrap(2 * std::atan2(leader.rotation[2], leader.rotation[3]) -
                            2 * std::atan2(support.rotation[2], support.rotation[3]));
    const float yaw = Wrap(self.yaw - turn), s = std::sin(yaw), c = std::cos(yaw);
    const Pose target{self.x - ((*relative)[0] * c + (*relative)[1] * s),
        self.y - (-(*relative)[0] * s + (*relative)[1] * c), self.z, yaw};
    return Valid(target) ? std::optional<Pose>(target) : std::nullopt;
}
}
void Check(bool ok, const char* msg) {
    if (!ok) {
        std::cerr << msg << '\n';
        std::exit(1);
    }
}
int main() {
    {
        // 2026-10-28 #1: the victim before -2.5397, goal -2.2104 (18.9 degrees), the engine left it at -2.3875.
        const float before = 3.74350f - 2 * pi, goal = -2.21036f, engine = 3.89568f - 2 * pi;
        Check(!PivotYaw(before, goal, engine, 0.3f), "a turn the engine made further is kept");
        const auto mid = PivotYaw(before, goal, engine, 0.75f);
        Check(mid && std::abs(Wrap(*mid - (before + Wrap(goal - before) * 0.75f))) < 1e-4f,
            "past the engine's turn the pivot follows the blend");
        const auto end = PivotYaw(before, goal, engine, 1.0f);
        Check(end && std::abs(Wrap(*end - goal)) < 1e-4f, "at the end the pivot stands at its goal heading");
        Check(!PivotYaw(before, goal, goal, 1.0f), "at its goal nothing is written");
        Check(!PivotYaw(1.0f, 1.0f, 0.5f, 1.0f), "no turn asked, none made (a player victim keeps its heading)");
        // The other way round and across the seam at +-pi.
        const auto left = PivotYaw(3.0f, -3.0f, 3.0f, 1.0f);
        Check(left && std::abs(Wrap(*left + 3.0f)) < 1e-4f, "the short way across +-pi");
        const auto half = PivotYaw(3.0f, -3.0f, 3.0f, 0.5f);
        Check(half && std::abs(std::abs(*half) - pi) < 1e-3f, "half way across the seam is +-pi");
        Check(!PivotYaw(0.2f, -0.2f, -0.3f, 0.5f), "an engine turn past the goal is not pulled back mid-blend");
    }
    using namespace AnimationInjector::NativePairRoles;
    const auto selection = [](std::initializer_list<Role> roles) {
        return Select(std::span(roles.begin(), roles.size()));
    };
    Check(selection({{1, true, false}, {2, true, true}}) == std::array<std::size_t, 2>{0, 1},
        "native target leader selected");
    Check(selection({{2, true, true}, {1, true, false}}) == std::array<std::size_t, 2>{1, 0},
        "actor array order not role order");
    Check(selection({{1, true, false}, {1, true, false}, {2, true, true}}) == std::array<std::size_t, 2>{0, 2},
        "first/third person entries share one physical role");
    Check(selection({{1, true, false}, {2, true, false}}) == std::array<std::size_t, 2>{1, 0},
        "native no-lead fallback uses first generator");
    Check(selection({{1, true, true}, {2, true, true}}) == std::array<std::size_t, 2>{1, 0},
        "multiple flags preserve first native leader");
    Check(!selection({{1, true, false}, {2, true, true}, {3, true, false}}),
        "three actors do not receive a two-role placement");
    Check(!selection({{1, true, false}, {1, true, true}}), "one physical actor is not a pair");
    Check(!selection({{1, true, false}, {0, true, true}}) && !selection({{1, true, false}, {2, false, true}}),
        "invalid actors/generators never touch transforms");
    using PairPlacementMathMark = AnimationInjector::PairPlacementMath::Mark;
    const PairPlacementMathMark leader{{0, 80, 0}, {0, 0, 1, 0}}, support{};
    unsigned cases = 0;
    for (float worldX : {0.f, 20000.f, -45000.f})
        for (float worldY : {0.f, 32000.f, -45929.f})
            for (float a : {-3.14f, -2.26214f, -1.f, 0.f, 1.f, 3.14f})
                for (float b : {-3.14f, -2.02f, -1.f, 0.f, 1.f, 3.14f}) {
                    std::array<Pose, 2> starts{{{worldX + 35, worldY + 25, -139, a}, {worldX, worldY, -140, b}}};
                    const auto p = Solve(starts, leader, support);
                    Check(p.has_value(), "bounded behind-chicken plan");
                    const auto x = p->goals[0], y = p->goals[1];
                    Check(std::abs(Wrap(x.yaw - y.yaw - pi)) < .00001f, "authored opposite facing");
                    Check(std::abs(x.x - y.x - 80 * std::sin(y.yaw)) < .01f &&
                              std::abs(x.y - y.y - 80 * std::cos(y.yaw)) < .01f,
                        "native relative mark preserved");
                    Check(std::abs(x.x + y.x - starts[0].x - starts[1].x) < .02f &&
                              std::abs(x.y + y.y - starts[0].y - starts[1].y) < .02f,
                        "midpoint preserved");
                    for (unsigned i = 0; i < 2; ++i) {
                        Check(Distance(starts[i], p->goals[i]) < 100, "bounded correction");
                        Check(Distance(Interpolate(starts[i], p->goals[i], 0), starts[i]) < .01f, "continuous start");
                        Check(Distance(Interpolate(starts[i], p->goals[i], 1), p->goals[i]) < .01f,
                            "continuous endpoint");
                    }
                    ++cases;
                }
    for (float yaw : {-pi, -1.f, 0.f, 1.f, pi}) {
        const Pose b{10000, -40000, 5, yaw}, a{b.x + 80 * std::sin(yaw), b.y + 80 * std::cos(yaw), 5, Wrap(yaw + pi)};
        const auto p = Solve({a, b}, leader, support);
        Check(p && p->cost < .001f, "aligned pair does not move");
        const auto target = TargetFor(a, leader, support);
        Check(target && Distance(*target, b) < .01f && std::abs(Wrap(target->yaw - b.yaw)) < .00001f,
            "release shares exact pickup mark frame");
    }
    // Victim pivot (0.12.256): the anchor (the victim, the scene's lead = role 1 in game) keeps its exact
    // position and only turns; the other role keeps its bearing from the anchor and moves only by the
    // difference between its current distance and the authored one.
    const auto bearingKept = [](const std::array<Pose, 2>& starts, const Plan& p, unsigned anchor) {
        const unsigned other = 1 - anchor;
        const float sx = starts[other].x - starts[anchor].x, sy = starts[other].y - starts[anchor].y;
        const float gx = p.goals[other].x - p.goals[anchor].x, gy = p.goals[other].y - p.goals[anchor].y;
        const float cross = sx * gy - sy * gx, dot = sx * gx + sy * gy;
        return std::abs(cross) <= .001f * std::hypot(sx, sy) * std::hypot(gx, gy) && dot > 0;
    };
    unsigned pivoted = 0;
    for (float worldX : {0.f, 20000.f, -45000.f})
        for (float a : {-3.14f, -1.f, 0.f, 1.f, 3.14f})
            for (float b : {-3.14f, -1.f, 0.f, 2.f})
                for (float distance : {20.f, 43.f, 80.f, 150.f})
                    for (float bearing : {-2.5f, -.3f, 0.f, 1.2f, 3.1f})
                        for (unsigned anchor : {0u, 1u}) {
                            const Pose victim{worldX, -45929.f, -140, anchor ? b : a};
                            const Pose other{victim.x + distance * std::sin(bearing),
                                victim.y + distance * std::cos(bearing), -139, anchor ? a : b};
                            std::array<Pose, 2> starts{};
                            starts[anchor] = victim;
                            starts[1 - anchor] = other;
                            const auto p = Pivoted(starts, leader, support, anchor);
                            Check(p.has_value(), "pivot plan");
                            const auto& kept = p->goals[anchor];
                            Check(kept.x == victim.x && kept.y == victim.y && kept.z == victim.z,
                                "victim keeps its exact position");
                            const auto x = p->goals[0], y = p->goals[1];
                            Check(std::abs(Wrap(x.yaw - y.yaw - pi)) < .00001f, "pivot: authored opposite facing");
                            Check(std::abs(x.x - y.x - 80 * std::sin(y.yaw)) < .01f &&
                                      std::abs(x.y - y.y - 80 * std::cos(y.yaw)) < .01f,
                                "pivot: native relative mark preserved");
                            Check(bearingKept(starts, *p, anchor), "attacker keeps its bearing from the victim");
                            const auto& moved = p->goals[1 - anchor];
                            Check(std::abs(std::hypot(moved.x - other.x, moved.y - other.y) - std::abs(distance - 80)) <
                                      .05f,
                                "attacker moves only the distance difference");
                            ++pivoted;
                        }
    for (float yaw : {-pi, -1.f, 0.f, 1.f, pi}) {
        const Pose b{10000, -40000, 5, yaw}, a{b.x + 80 * std::sin(yaw), b.y + 80 * std::cos(yaw), 5, Wrap(yaw + pi)};
        const auto p = Pivoted({a, b}, leader, support, 1);
        Check(
            p && p->cost < .001f && Distance(p->goals[0], a) < .01f && std::abs(Wrap(p->goals[1].yaw - b.yaw)) < .0001f,
            "aligned pair neither moves nor turns");
    }
    {
        const auto p = Pivoted({Pose{0, 0, 0, 0}, Pose{50, 10, 0, 1}}, leader, support, 1);
        const float bearing = std::atan2(-50.f, -10.f), length = std::hypot(50.f, 10.f);
        Check(p && p->goals[1].x == 50 && p->goals[1].y == 10 && std::abs(Wrap(p->goals[1].yaw - bearing)) < .00001f,
            "victim stays and turns to face the attacker");
        Check(p && std::abs(p->goals[0].x - (50 - 80 * 50 / length)) < .01f &&
                  std::abs(p->goals[0].y - (10 - 80 * 10 / length)) < .01f &&
                  std::abs(Wrap(p->goals[0].yaw - bearing - pi)) < .00001f,
            "attacker steps back to 80 on the same line, facing the victim");
        // No bearing (same spot): the victim also keeps its heading = the old full hold.
        const auto same = Pivoted({Pose{7, 7, 0, 2}, Pose{7, 7, 0, 1}}, leader, support, 1);
        Check(same && same->goals[1].yaw == 1 && std::abs(same->goals[0].x - 7 - 80 * std::sin(1.f)) < .01f,
            "coincident roles keep the victim's heading");
        const auto front = Pivoted({Pose{0, 0, 0, 0}, Pose{0, 0, 0, 0}}, leader, support, 0);
        const auto target = TargetFor(Pose{0, 0, 0, 0}, leader, support);
        Check(front && target && Distance(front->goals[1], *target) < .01f, "coincident anchor 0 equals the release");
        Check(!Pivoted({Pose{}, Pose{}}, leader, support, 2), "anchor index out of range rejected");
        auto bad = Pose{};
        bad.yaw = std::numeric_limits<float>::quiet_NaN();
        Check(!Pivoted({bad, Pose{}}, leader, support, 0), "nonfinite victim pose rejected");
        const auto unknown = Pivoted({bad, Pose{0, 0, 0, 1}}, leader, support, 1);
        Check(unknown && unknown->goals[1].yaw == 1, "unknown attacker pose keeps the victim's heading");
        // A player victim keeps its heading (0.12.262): the attacker takes the authored place around it.
        const auto held = Pivoted({Pose{0, 0, 0, 0}, Pose{50, 10, 0, 1}}, leader, support, 1, true);
        Check(held && held->goals[1].x == 50 && held->goals[1].y == 10 && held->goals[1].yaw == 1,
            "kept heading: the victim neither moves nor turns");
        Check(held && std::abs(held->goals[0].x - (50 + 80 * std::sin(1.f))) < .01f &&
                  std::abs(held->goals[0].y - (10 + 80 * std::cos(1.f))) < .01f &&
                  std::abs(Wrap(held->goals[0].yaw - 1 - pi)) < .00001f,
            "kept heading: the attacker stands 80 in front of the victim's own facing, facing it");
    }
    unsigned releases = 0;
    unsigned genericPlans = 0;
    for (float distance : {25.f, 80.f, 160.f})
        for (float markerYaw : {-2.f, 0.f, .4f, pi})
            for (float bodyYaw : {-3.1f, -.8f, 0.f, 1.f, 3.1f}) {
                PairPlacementMathMark lead{
                    {12, distance, 0}, {0, 0, std::sin(markerYaw * .5f), std::cos(markerYaw * .5f)}};
                PairPlacementMathMark sup{{5, -9, 0}, {0, 0, std::sin(.15f), std::cos(.15f)}};
                const Pose self{21321, -49876, -142, bodyYaw};
                const auto target = TargetFor(self, lead, sup);
                Check(target.has_value(), "non-opposite, non-80 release supported");
                const auto r = AnimationInjector::PairPlacementMath::RelativePosition(lead, sup);
                const float s = std::sin(target->yaw), c = std::cos(target->yaw);
                Check(std::abs(target->x + (*r)[0] * c + (*r)[1] * s - self.x) < .01f &&
                          std::abs(target->y - (*r)[0] * s + (*r)[1] * c - self.y) < .01f,
                    "release reconstructs native initiator position");
                Check(std::abs(Wrap(self.yaw - target->yaw - markerYaw + .3f)) < .00001f,
                    "release preserves authored relative heading");
                const std::array<Pose, 2> start{{{self.x + 10, self.y - 8, self.z, Wrap(self.yaw + .2f)},
                    {target->x - 12, target->y + 6, target->z, Wrap(target->yaw - .1f)}}};
                const auto plan = Solve(start, lead, sup);
                Check(plan.has_value(), "non-80/non-opposite pickup plan supported");
                const auto expectedTarget = TargetFor(plan->goals[0], lead, sup);
                Check(expectedTarget && Distance(*expectedTarget, plan->goals[1]) < .02f &&
                          std::abs(Wrap(expectedTarget->yaw - plan->goals[1].yaw)) < .00001f,
                    "generic pickup agrees with native release frame");
                const auto pivot = Pivoted(start, lead, sup, 1);
                Check(pivot && pivot->goals[1].x == start[1].x && pivot->goals[1].y == start[1].y &&
                          bearingKept(start, *pivot, 1),
                    "non-80/non-opposite marks: victim stays, attacker keeps its bearing");
                const auto pivotTarget = TargetFor(pivot->goals[0], lead, sup);
                Check(pivotTarget && Distance(*pivotTarget, pivot->goals[1]) < .02f &&
                          std::abs(Wrap(pivotTarget->yaw - pivot->goals[1].yaw)) < .0001f,
                    "pivot agrees with native release frame");
                ++genericPlans;
                ++releases;
            }
    auto bad = leader;
    bad.rotation = {0, 0, 0, 0};
    Check(!Solve({Pose{}, Pose{}}, bad, support), "invalid marks rejected");
    bad = leader;
    bad.rotation = {.70710678f, 0, 0, .70710678f};
    Check(!Solve({Pose{}, Pose{}}, bad, support), "nonplanar marks rejected");
    const auto large = Solve({Pose{}, Pose{10000, 0, 0, 0}}, leader, support);
    Check(large && Distance(Pose{}, large->goals[0]) > 100, "native-approved distance has no second cutoff");
    for (float height : {-100.f, -8.56f, 0.f, 8.f, 8.56f, 100.f}) {
        const auto uneven = Solve({Pose{0, 0, height, 0}, Pose{0, 80, -140, pi}}, leader, support);
        Check(uneven && uneven->goals[0].z == -140 && uneven->goals[1].z == -140,
            "native leader height wins without an 8-unit terrain gate or height averaging");
    }
    // Exact failing 0.12.114 pickup: native accepted, custom height guard skipped.
    const std::array<Pose, 2> reported{
        {{22169.656f, -44633.906f, -132.853f, 1.69660f}, {22278.693f, -44616.840f, -141.412f, 2.85034f}}};
    const auto repaired = Solve(reported, leader, support);
    Check(repaired && repaired->goals[0].z == reported[1].z && repaired->goals[1].z == reported[1].z,
        "reported 8.559-unit height difference keeps bilateral correction");
    for (float height : {-120.f, 25.f, 200.f}) {
        auto raised = leader;
        raised.translation[2] = height;
        const auto vertical = Solve(reported, raised, support);
        const auto relative = AnimationInjector::PairPlacementMath::RelativePosition(raised, support);
        Check(vertical && relative && std::abs(vertical->goals[0].z - vertical->goals[1].z - (*relative)[2]) < .001f,
            "authored vertical mark offset is preserved instead of flattened");
    }
    auto invalidPose = Pose{};
    invalidPose.x = std::numeric_limits<float>::quiet_NaN();
    Check(!Solve({invalidPose, Pose{}}, leader, support), "nonfinite actor pose still rejected");
    Gate g;
    Check(!g.Step(0, true, 0, 0) && g.elapsed == 0, "pause cannot advance gate");
    Check(!g.Step(.02f, true, 0, .1f) && g.settled == 0, "yaw mismatch blocks native");
    Check(!g.Step(.02f, false, 0, 0) && g.settled == 0, "missing model blocks native");
    Check(!g.Step(.02f, true, 3, 0) && g.settled == 0, "position mismatch blocks native");
    Check(!g.Step(.02f, true, 0, 0), "one frame insufficient");
    Check(g.Step(.02f, true, 0, 0), "independent stable frames release");
    const auto p =
        Solve({Pose{20695.80f, -45904.53f, -138.81f, 4.02105f}, Pose{20660.63f, -45929.64f, -140.02f, -2.0208f}},
            leader, support);
    Check(p.has_value(), "reported second pickup has valid shared plan");
    // 0.12.279: the start blend puts back a snapped role turned more than about a degree (pair #6: -0.45659).
    Check(HeadingDrifted(-.45659f) && HeadingDrifted(.021f) && HeadingDrifted(-.021f), "a turned heading is restored");
    Check(!HeadingDrifted(.0f) && !HeadingDrifted(.02f) && !HeadingDrifted(-.019f), "a heading within slack stays");
    Check(!HeadingDrifted(std::numeric_limits<float>::quiet_NaN()), "an unreadable heading is left alone");
    for (unsigned i = 0; i < 2; ++i)
        std::cout << "reported role=" << i << " goal=" << p->goals[i].x << ',' << p->goals[i].y << ',' << p->goals[i].z
                  << " yaw=" << p->goals[i].yaw << '\n';
    std::cout << "PASS: " << cases << " shared plans, " << pivoted << " victim-pivot plans, " << genericPlans
              << " generic pickup plans, " << releases
              << " mark-based releases, stationary pairs, bounded interpolation, yaw/position handoff gate\n";
}
