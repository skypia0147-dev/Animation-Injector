#pragma once
#include <array>
#include <cmath>
#include <optional>

namespace AnimationInjector::SpaceQueryMath {
using Point = std::array<float, 3>;
// Keep the collider-to-controller offset; the native destination uses game units.
inline std::optional<Point> Origin(Point shape, Point feet, Point destination, float scale) {
    if (!std::isfinite(scale) || scale <= 0) return {};
    Point result;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(shape[i]) || !std::isfinite(feet[i]) || !std::isfinite(destination[i])) return {};
        result[i] = shape[i] + destination[i] * scale - feet[i];
        if (!std::isfinite(result[i])) return {};
    }
    return result;
}
}
