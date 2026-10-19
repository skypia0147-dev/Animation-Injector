#pragma once
#include <cmath>
#include <limits>

namespace AnimationInjector::PairTargetMath {
// Actor yaw can accumulate more than one revolution during authored movement.
inline float AbsoluteHeading(float degrees) {
    return std::isfinite(degrees) ? std::fabs(std::remainder(degrees, 360.0f)) : std::numeric_limits<float>::infinity();
}
inline bool WithinRangeAndCone(float distance, float range, float heading, float angle) {
    return std::isfinite(distance) && distance >= 0 && std::isfinite(range) && range >= 0 && distance <= range &&
           std::isfinite(angle) && angle >= 0 && AbsoluteHeading(heading) <= angle * .5f;
}
}
