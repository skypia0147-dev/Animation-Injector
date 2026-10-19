#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace AnimationInjector {
// A bounded exit request may cross callbacks and graph contexts, not new actions.
template<class Owner>
struct LocomotionDestinationWindow {
    Owner owner{};
    float remaining = 0;
    std::array<std::uintptr_t, 4> consumed{};
    void Reset() {
        owner = {};
        remaining = 0;
        consumed = {};
    }
    void Arm(const Owner& value) {
        if (remaining > 0 && owner == value) return;
        Reset();
        owner = value;
        remaining = 0.35f;
    }
    void Tick(float delta) {
        if (std::isfinite(delta) && delta > 0 && remaining > 0) {
            remaining -= delta;
            if (remaining <= 0) Reset();
        }
    }
    bool Consume(const Owner& value, std::uintptr_t context) {
        if (remaining <= 0 || !(owner == value) || !context) return false;
        for (auto id : consumed)
            if (id == context) return false;
        for (auto& id : consumed)
            if (!id) {
                id = context;
                return true;
            }
        return false;
    }
};
}
