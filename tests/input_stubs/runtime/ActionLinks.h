#pragma once
#include "PCH.h"
#include "runtime/GameControls.h"
#include "runtime/HeldKeys.h"
#include "runtime/PressKinds.h"
#include <vector>
namespace AnimationInjector::ActionLinks {
inline int calls = 0;
struct InputResult {
    bool claimed = false, taken = false, kept = false;
};
enum class InputPhase : std::uint8_t { kDown, kHeld, kUp };
inline InputResult Input(
    std::uint32_t, bool, GameControl = GameControl::kNone, InputPhase = InputPhase::kDown, float = -1.0f) {
    ++calls;
    return {};
}
struct Forward {
    HeldKeys::Key key;
    PressKinds::Press press = PressKinds::Press::kDown;
    float downAt = 0;
    int direction = -1;
};
inline std::vector<Forward> forwards;
inline std::vector<Forward> TakeForwards() { return std::exchange(forwards, {}); }
inline void DropPress(HeldKeys::Key) {}
}
