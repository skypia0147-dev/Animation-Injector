#include "core/NativePairMarkCapture.h"
#include "runtime/PairAlignmentMath.h"
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace AnimationInjector;
void Check(bool ok, const char* message) {
    if (!ok) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
int main() {
    const NativePairMarkCapture::Identity support{10, 100}, leader{30, 300};
    const PairPlacementMath::Mark supportMark{}, leaderMark{{0, 80, 0}, {0, 0, 1, 0}};
    for (bool reverse : {false, true}) {
        NativePairMarkCapture::Capture captured;
        captured.identities = {support, leader};
        Check(!captured.Ready(true), "empty resident slot must wait for native marks");
        captured.Record(reverse ? support : leader, true, reverse ? supportMark : leaderMark);
        Check(!captured.Ready(true), "one successful mark cannot align two actors");
        captured.Record(reverse ? leader : support, true, reverse ? leaderMark : supportMark);
        Check(captured.Ready(true) && !captured.Ready(false), "engine rejection must not publish fallback");
        const auto marks = captured.SolverMarks();
        Check(marks[0].translation == leaderMark.translation && marks[1].translation == supportMark.translation,
            "native roles preserve solver order");
        const std::array<PairAlignmentMath::Pose, 2> before{{{0, 0, 0, 0}, {0, 80, 0, 3.14159265f}}};
        const auto direct = PairAlignmentMath::Solve(before, leaderMark, supportMark);
        const auto fallback = PairAlignmentMath::Solve(before, marks[0], marks[1]);
        Check(direct.has_value() && fallback.has_value(), "resident and native paths both produce placement");
        for (unsigned i = 0; i < 2; ++i)
            Check(PairAlignmentMath::Distance(direct->goals[i], fallback->goals[i]) < .00001f &&
                      direct->goals[i].yaw == fallback->goals[i].yaw,
                "unload after native sampling cannot change copied placement");
        Check(captured.Record({11, 100}, true, supportMark) == -1,
            "duplicate first-person generator cannot replace selected physical entry");
        Check(captured.Record({10, 101}, true, supportMark) == -1, "another character cannot supply selected mark");
        Check(captured.Ready(true), "unrelated observations leave complete marks intact");
        auto other = supportMark;
        other.translation[0] = 5;
        captured.Record(support, true, other);
        Check(!captured.Ready(true), "conflicting duplicate marks fail closed");
    }
    NativePairMarkCapture::Capture failed;
    failed.identities = {support, leader};
    failed.Record(leader, true, leaderMark);
    failed.Record(support, false, supportMark);
    failed.Record(support, true, supportMark);
    Check(!failed.Ready(true), "native failure cannot be overwritten by later success");
    NativePairMarkCapture::Capture invalid;
    invalid.identities = {support, leader};
    auto nan = supportMark;
    nan.translation[1] = std::numeric_limits<float>::quiet_NaN();
    invalid.Record(support, true, nan);
    invalid.Record(leader, true, leaderMark);
    Check(!invalid.Ready(true), "nonfinite native mark rejected");
    NativePairMarkCapture::Capture next;
    next.identities = {support, leader};
    Check(!next.Ready(true), "new or nested start cannot reuse another scene marks");
    std::cout
        << "Native mark fallback: reversed order, unloaded binding, duplicate views, rejected/invalid/incomplete marks and independent starts passed\n";
}
