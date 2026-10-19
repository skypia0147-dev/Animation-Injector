#pragma once
#include <cmath>
namespace AnimationInjector {
// Movement ownership relaxes only the old stationary-anchor guard.
inline bool PairRootExitMayContinue(
    bool sameActor, bool locomotion, float distance, float yawDifference, float elapsed, float budget) {
    if (!sameActor || !std::isfinite(distance) || !std::isfinite(yawDifference) || !std::isfinite(elapsed) ||
        !std::isfinite(budget) || elapsed > budget)
        return false;
    return locomotion || (distance <= 5 && std::fabs(std::remainder(yawDifference, 6.283185307f)) <= .03f);
}
}
