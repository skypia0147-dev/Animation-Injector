#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace AnimationInjector::NativePairObservation {
// A reused generator address can still report the previous play's clock while
// native Start is queuing this scene. Only the activated scene owns samples.
struct Clip {
    float time = -1;
    bool ended = false;
    void Observe(bool activated, std::uintptr_t expectedScene, std::uintptr_t scene, bool exiting, float localTime) {
        if (!activated || !expectedScene || scene != expectedScene) return;
        ended |= exiting;
        if (!exiting && std::isfinite(localTime) && localTime >= 0) time = std::max(time, localTime);
    }
};
}
