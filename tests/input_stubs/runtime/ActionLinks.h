#pragma once
#include "PCH.h"
#include "runtime/GameControls.h"
namespace AnimationInjector::ActionLinks {
inline int calls = 0;
struct InputResult {
    bool claimed = false, taken = false;
};
inline InputResult Input(std::uint32_t, bool, GameControl = GameControl::kNone, bool = true) {
    ++calls;
    return {};
}
}
