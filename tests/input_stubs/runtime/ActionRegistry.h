#pragma once
#include <vector>
#include "runtime/ActionRunner.h"
namespace AnimationInjector::Actions {
inline ActionRunner runner;
inline int Count() { return 1; }
inline ActionRunner& Runner(int) { return runner; }
// The stub action's runner always exists (NextRunner walks it too).
inline ActionRunner* RunnerFor(RE::PlayerCharacter*, int, bool) { return &runner; }
inline const ActionSpec& Spec(int) { return runner.spec; }
inline int CaptureTarget(RE::PlayerCharacter*, int) { return 0; }
inline int SelectStart(RE::PlayerCharacter*, int a, int = 0) { return a; }
inline int SelectPairedStart(RE::PlayerCharacter*, int a, int) { return a; }
// The one stub action, indexed like the registry does from its trigger.
inline std::vector<int> keyed;
inline const std::vector<int>& ActionsForKey(std::uint32_t code) {
    keyed.clear();
    if (code == runner.spec.trigger.key) keyed.push_back(0);
    return keyed;
}
inline const std::vector<int>& ActionsForGamepad(std::uint32_t button) {
    keyed.clear();
    if (button && button == runner.spec.trigger.gamepad) keyed.push_back(0);
    return keyed;
}
// Its own list: the sink holds the key list and this one at once.
inline std::vector<int> controlled;
inline const std::vector<int>& ActionsForControl(GameControl control) {
    controlled.clear();
    if (control != GameControl::kNone && control == runner.spec.trigger.control) controlled.push_back(0);
    return controlled;
}
inline int NextRunner(RE::PlayerCharacter*, int after, ActionRunner** r) {
    *r = after < 0 ? &runner : nullptr;
    return after < 0 ? 0 : -1;
}
template<class F>
ActionRunner* FindRunner(RE::PlayerCharacter* p, F&& f) {
    ActionRunner* r = nullptr;
    for (int a = NextRunner(p, -1, &r); a >= 0; a = NextRunner(p, a, &r))
        if (f(a, *r)) return r;
    return nullptr;
}
}
