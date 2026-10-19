#include "runtime/CameraRecovery.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
#include <limits>

using AnimationInjector::CameraRecovery;
using Step = CameraRecovery::Step;

int main() {
    CameraRecovery state;
    assert(state.Update(1, false, false) == Step::kWait);
    state.Request();
    assert(state.Update(0, false, false) == Step::kApply);
    assert(state.Update(0.25f, false, true) == Step::kWait);
    assert(state.Update(0.25f, false, true) == Step::kComplete);

    // The original failure: an apparently restored camera is invalidated by ragdoll.
    state = {};
    state.Request();
    assert(state.Update(0, false, false) == Step::kApply);
    assert(state.Update(0.1f, false, true) == Step::kWait);
    assert(state.Update(1, true, true) == Step::kWait);
    for (int i = 0; i < 10; ++i) assert(state.Update(1, true, false) == Step::kWait);
    assert(state.attempts == 1 && !state.applied);
    // A single normal frame during get-up must not trigger restoration.
    assert(state.Update(0.1f, false, true) == Step::kWait);
    assert(state.Update(0.1f, true, true) == Step::kWait);
    assert(state.Update(0.125f, false, true) == Step::kWait);
    assert(state.Update(0.125f, false, true) == Step::kApply);
    assert(state.Update(0.25f, false, true) == Step::kWait);
    assert(state.Update(0.25f, false, true) == Step::kComplete);

    // The engine may report third person for one frame then restore the animated camera.
    state = {};
    state.Request();
    assert(state.Update(0, false, false) == Step::kApply);
    assert(state.Update(0.25f, false, true) == Step::kWait);
    assert(state.Update(0.25f, false, false) == Step::kApply);
    state.Request();  // Repeated end/reset events cannot reset the retry budget.
    assert(state.attempts == 2);
    for (int i = 2; i < 20; ++i) assert(state.Update(0.25f, false, false) == Step::kApply);
    assert(state.Update(0.25f, false, false) == Step::kExpired);

    state = {};
    state.Request();
    assert(state.Update(119, true, false) == Step::kWait);
    assert(state.Update(1, true, false) == Step::kExpired);
    state = {};  // Load/new shot drops all delayed work.
    assert(state.Update(1, false, false) == Step::kWait);
    state.Request();
    assert(state.Update(std::numeric_limits<float>::quiet_NaN(), false, false) == Step::kWait);
    assert(state.Update(-1, false, false) == Step::kWait);
    assert(state.age == 0 && state.attempts == 0);
    std::cout << "Camera recovery interruption/retry/load checks passed\n";
}
