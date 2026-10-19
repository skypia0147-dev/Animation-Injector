#pragma once
#include "runtime/CarryPlacement.h"

namespace AnimationInjector::CarryHandoff {
// One final playing sample, frozen before an exit blend changes either rig.
// This stores a root-to-anchor relation, not a scene offset: the root's local
// transform may change during cleanup and must be compensated at attachment.
template<class Transform>
struct Pose {
    Transform relative{};
    bool valid = false, frozen = false;
    unsigned samples = 0;

    void Sample(const Transform& anchor, const Transform& rootWorld) {
        if (frozen) return;
        relative = CarryPlacement::NativeOffset(anchor, rootWorld);
        valid = true;
        ++samples;
    }
    bool Freeze() {
        frozen = true;
        return valid;
    }
    Transform Root(const Transform& anchor, float rootWorldScale) const {
        return CarryPlacement::NativeScene(anchor, relative, rootWorldScale);
    }
};
}
