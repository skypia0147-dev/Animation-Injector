#pragma once
#include "PCH.h"
namespace AnimationInjector {
enum class MoveDir { kNone, kForward, kBack, kLeft, kRight };
struct ActionSpec {
    struct Trigger {
        std::uint32_t key = 34, gamepad = 1;
        bool onKeyUp = false;
    } trigger;
    struct Paired {
        bool enabled = false;
    } paired;
};
}
