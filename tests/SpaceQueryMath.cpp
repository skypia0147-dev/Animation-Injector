#include "core/SpaceQueryMath.h"
#include "core/SpaceContactPolicy.h"
#include "core/PairPlacementMath.h"
#include "core/GroundPolicy.h"
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace AnimationInjector::SpaceQueryMath;
void Check(bool ok) {
    if (!ok) std::abort();
}
int main() {
    using namespace AnimationInjector::PairPlacementMath;
    Check(RelativePosition(Mark{}, Mark{{100, 20, 3}}) == std::optional<Vector>({100, 20, 3}));
    Check(RelativePosition(Mark{{10, 20, 30}}, Mark{{10, 20, 30}}) == std::optional<Vector>({0, 0, 0}));
    const float half = std::sqrt(0.5f);
    auto rotated = RelativePosition(Mark{{10, 0, 0}}, Mark{{0, 0, 0}, {0, 0, half, half}});
    Check(rotated && std::abs((*rotated)[0]) < .0001f && std::abs((*rotated)[1] + 10) < .0001f);
    auto inverse = RelativePosition(Mark{{10, 0, 0}, {0, 0, half, half}}, Mark{});
    Check(inverse && std::abs((*inverse)[0]) < .0001f && std::abs((*inverse)[1] - 10) < .0001f);
    Check(!RelativePosition(Mark{{}, {0, 0, 0, 0}}, Mark{}));
    Check(!RelativePosition(Mark{{std::numeric_limits<float>::infinity(), 0, 0}}, Mark{}));
    // The physical shape center must retain its height above the actor's feet.
    auto p = Origin({10, 20, 37}, {10, 20, 30}, {150, 250, 400}, 0.1f);
    Check(
        p && std::abs((*p)[0] - 15) < 0.0001f && std::abs((*p)[1] - 25) < 0.0001f && std::abs((*p)[2] - 47) < 0.0001f);
    // The stationary participant must remain at the original collider position.
    Check(Origin({10, 20, 37}, {10, 20, 30}, {100, 200, 300}, 0.1f) == std::optional<Point>({10, 20, 37}));
    Check(!Origin({0, 0, 0}, {0, 0, 0}, {1, 2, 3}, 0));
    Check(!Origin({0, 0, 0}, {0, 0, 0}, {1, 2, 3}, -1));
    const float invalid = std::numeric_limits<float>::quiet_NaN();
    Check(!Origin({0, invalid, 0}, {0, 0, 0}, {1, 2, 3}, 1));
    Check(!Origin({0, 0, 0}, {0, 0, invalid}, {1, 2, 3}, 1));
    Check(!Origin({0, 0, 0}, {0, 0, 0}, {invalid, 2, 3}, 1));
    Check(!Origin({0, 0, 0}, {0, 0, 0}, {1, 2, 3}, invalid));
    std::cout << "SpaceQueryMath: passed\n";
    using namespace AnimationInjector;
    using Surface = SpaceContactPolicy::Surface;
    using Value = ConditionFlow::Value;
    const GroundPolicy::Limits ground;
    Check(GroundPolicy::Judge(true, true, 0, 0, 0, 1, ground) == Value::kTrue);
    Check(GroundPolicy::Judge(false, false, 0, 0, 0, 0, ground) == Value::kFalse);
    Check(GroundPolicy::Judge(true, false, 0, 0, 0, 1, ground) == Value::kUnavailable);
    Check(GroundPolicy::Judge(true, true, 21, 0, 0, 1, ground) == Value::kFalse);
    Check(GroundPolicy::Judge(true, true, -41, 0, 0, 1, ground) == Value::kFalse);
    Check(GroundPolicy::Judge(true, true, 20, 0, 0, 1, ground) == Value::kTrue);
    Check(GroundPolicy::Judge(true, true, -40, 0, 0, 1, ground) == Value::kTrue);
    Check(GroundPolicy::Judge(true, true, 0, 0.8660254f, 0, 0.5f, ground) == Value::kFalse);
    Check(GroundPolicy::Judge(true, true, 0, 0.5f, 0, 0.8660254f, ground) == Value::kTrue);
    Check(GroundPolicy::Judge(true, true, 0, 0, 0, -1, ground) == Value::kFalse);
    Check(GroundPolicy::Judge(true, true, invalid, 0, 0, 1, ground) == Value::kUnavailable);
    Check(!GroundPolicy::Valid({20, 40, 90, 20}));
    SpaceContactPolicy::Summary contacts;
    contacts.Add(Surface::kParticipant, 0, 70);
    contacts.Add(Surface::kNonPhysical, invalid, invalid);
    contacts.Add(Surface::kFixed, 1, 0);
    Check(contacts.Result(true) == Value::kTrue);
    contacts.Add(Surface::kFixed, 0, 40);
    Check(contacts.Result(true) == Value::kFalse);
    contacts.Add(Surface::kUnknown, 1, 0);
    Check(contacts.Result(true) == Value::kUnavailable);
    SpaceContactPolicy::Summary raised;
    raised.Add(Surface::kFixed, 1, 70);
    Check(raised.Result(true) == Value::kFalse);
    Check(raised.Result(false) == Value::kUnavailable);
    SpaceContactPolicy::Summary malformed;
    malformed.Add(Surface::kFixed, invalid, 0);
    Check(malformed.Result(true) == Value::kUnavailable);
    // The same verdict type feeds the existing pass/fail evaluator, including unknown failures.
    using Kind = ConditionFlow::Kind;
    const std::vector<ConditionFlow::Node> flow{
        {Kind::kTest, 0, {}, 1, 2}, {Kind::kAction, -1, {}, -1, -1, 0}, {Kind::kAction, -1, {}, -1, -1, 1}};
    for (auto v : {Value::kTrue, Value::kFalse, Value::kUnavailable}) {
        const auto choice = ConditionFlow::Select(flow, 0, 1, 2, [&](int) { return v; });
        Check(choice.action == (v == Value::kTrue ? 0 : 1));
        Check(choice.unavailable == (v == Value::kUnavailable));
    }
    // Center and feet must come from the same controller snapshot, not a lagging physics pose.
    auto aligned = Origin({100, 200, 307}, {100, 200, 300}, {500, 600, 700}, 1);
    Check(aligned && (*aligned == Point{500, 600, 707}));
    std::cout << "SpaceContactPolicy and condition routing: passed\n";
}
