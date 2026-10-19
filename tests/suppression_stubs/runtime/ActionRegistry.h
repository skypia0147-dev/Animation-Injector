#pragma once
#include <memory>
#include "runtime/ActionRunner.h"
namespace AnimationInjector::Actions {
inline std::vector<ActionSpec> specs(2);
inline std::vector<ActionRunner> runners(2);
inline int selected = -1, queries = 0;
inline bool chance = false;
inline bool HasChance(int) { return chance; }
inline int Count() { return static_cast<int>(specs.size()); }
inline ActionSpec& Spec(int a) { return specs[a]; }
inline ActionRunner* RunnerFor(RE::Actor*, int a, bool) { return a >= 0 && a < Count() ? &runners[a] : nullptr; }
// Same lookup as RunnerFor; the stub runners outlive the test, so the reference does not own them.
inline std::shared_ptr<ActionRunner> RunnerRef(RE::Actor* actor, int a) {
    auto* r = RunnerFor(actor, a, false);
    return r ? std::shared_ptr<ActionRunner>(std::shared_ptr<void>{}, r) : nullptr;
}
// Every stub runner exists; walked in action order like the registry's existing runners.
template<class F>
ActionRunner* FindRunner(RE::Actor*, F&& f) {
    for (int a = 0; a < Count(); ++a)
        if (f(a, runners[a])) return &runners[a];
    return nullptr;
}
inline int SelectStart(RE::Actor*, int) {
    ++queries;
    return selected;
}
}
