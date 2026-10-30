#pragma once
#include "PCH.h"
namespace AnimationInjector {
enum class MoveDir { kNone, kBack };
struct SuppressSpec {
    enum class When { kStartable, kActive, kBoth };
    When when = When::kStartable;
    std::vector<std::string> events;
};
struct ActionSpec {
    SuppressSpec suppress;
    struct Trigger {
        std::string onEvent;
        bool onEventNpc = true;
    } trigger;
};
struct ActionRunner {
    bool active = false;
    int origin = -1;
    bool IsActive() const { return active; }
    int RequestOrigin() const { return origin; }
};
}
