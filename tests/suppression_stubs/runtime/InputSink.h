#pragma once
#include "runtime/ActionRunner.h"
namespace AnimationInjector {
struct InputSink {
    MoveDir direction = MoveDir::kBack;
    static InputSink* Get() {
        static InputSink s;
        return &s;
    }
    MoveDir CurrentMoveDir(MoveDir) const { return direction; }
};
}
