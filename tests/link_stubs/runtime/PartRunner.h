#pragma once
#include "LinkHarness.h"
// What ActionLinks asks of part actions (runtime/PartRunner.h), recorded.
namespace AnimationInjector::PartRunner {
struct Calls {
    bool allow = true;
    int links = 0, ends = 0;
    int linkSource = -1, linkTarget = -1, endAction = -1;
    float endBlend = -2.0f;
};
inline Calls calls;
inline bool CanLink(RE::Actor*, int, int) { return calls.allow; }
inline bool Link(RE::Actor*, int source, int target) {
    ++calls.links;
    calls.linkSource = source;
    calls.linkTarget = target;
    return calls.allow;
}
inline bool EndAction(RE::Actor*, int action, const char*, float blend = -1.0f) {
    ++calls.ends;
    calls.endAction = action;
    calls.endBlend = blend;
    return true;
}
}  // namespace AnimationInjector::PartRunner
