#pragma once
#include "runtime/ConditionFlow.h"
#include <cmath>

namespace AnimationInjector::GroundPolicy {
struct Limits {
    float rise = 20, drop = 40, slope = 45, radius = 20;
};
inline bool Valid(Limits p) {
    return std::isfinite(p.rise) && p.rise >= 0 && p.rise <= 200 && std::isfinite(p.drop) && p.drop >= 0 &&
           p.drop <= 500 && std::isfinite(p.slope) && p.slope >= 0 && p.slope <= 80 && std::isfinite(p.radius) &&
           p.radius > 0 && p.radius <= 100;
}
inline ConditionFlow::Value Judge(bool hit, bool fixed, float height, float nx, float ny, float nz, Limits p) {
    using V = ConditionFlow::Value;
    if (!Valid(p)) return V::kUnavailable;
    if (!hit) return V::kFalse;
    if (!fixed || !std::isfinite(height) || !std::isfinite(nx) || !std::isfinite(ny) || !std::isfinite(nz))
        return V::kUnavailable;
    const float norm = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (norm < 0.99f || norm > 1.01f) return V::kUnavailable;
    return height <= p.rise + 0.01f && height >= -p.drop - 0.01f &&
                   nz / norm + 0.00001f >= std::cos(p.slope * 0.01745329252f)
               ? V::kTrue
               : V::kFalse;
}
}
