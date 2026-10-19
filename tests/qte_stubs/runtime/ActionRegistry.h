#pragma once
#include "runtime/QteSpec.h"
namespace AnimationInjector::Actions {
struct TestSpec {
    std::string id;
    std::vector<QteSpec> qtes;
};
extern std::vector<TestSpec> specs;
inline int Count() { return static_cast<int>(specs.size()); }
inline const TestSpec& Spec(int i) { return specs[i]; }
bool QueueEventTrigger(RE::Actor*, const RE::BSFixedString&);
void OnAnimEventFrom(RE::Actor*, const RE::BSFixedString&);
}
