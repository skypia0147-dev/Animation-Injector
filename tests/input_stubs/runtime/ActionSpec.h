#pragma once
#include "PCH.h"
#include "runtime/GameControls.h"
#include "runtime/PressKinds.h"
namespace AnimationInjector {
enum class MoveDir { kNone, kForward, kBack, kLeft, kRight };
struct ActionSpec {
    struct Trigger {
        std::uint32_t key = 34, gamepad = 1;
        GameControl control = GameControl::kNone;
        PressKinds::Press press = PressKinds::Press::kDown;
    } trigger;
    struct Paired {
        bool enabled = false;
    } paired;
};
}
