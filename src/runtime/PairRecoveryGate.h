#pragma once
#include <cmath>

namespace AnimationInjector {
// Fed only by the initiating clip's clock/event; menu time is never a clock.
struct PairRecoveryGate {
    bool enabled = false, open = false, consumed = false;
    float at = -1.0f;
    void ObserveClock(float seconds) {
        if (enabled && at >= 0 && std::isfinite(seconds) && seconds >= at) open = true;
    }
    void ObserveEvent() {
        if (enabled) open = true;
    }
    bool Consume(bool playing, bool moving) {
        if (!enabled || !open || consumed || !playing || !moving) return false;
        consumed = true;
        return true;
    }
};
}
