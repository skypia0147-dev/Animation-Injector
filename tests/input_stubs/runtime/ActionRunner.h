#pragma once
#include "runtime/ActionSpec.h"
namespace AnimationInjector {
class ActionRunner {
public:
    enum class State { kIdle, kActive };
    State state = State::kIdle;
    ActionSpec spec;
    int moves = 0, windows = 0, requests = 0;
    void OnMoveInput(int) { ++moves; }
    void OnWindowInput(std::uint32_t, bool, bool) { ++windows; }
    State GetState() const { return state; }
    const ActionSpec& Spec() const { return spec; }
    bool ValidPairedTarget(int) const { return true; }
    void RequestPaired(int) { ++requests; }
    int lastOrigin = -1;
    bool Request(int origin = -1) {
        ++requests;
        lastOrigin = origin;
        return true;
    }
};
}
