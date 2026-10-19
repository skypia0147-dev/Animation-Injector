#include "runtime/DrivenFlagOwnership.h"
#include "runtime/LifecyclePolicy.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
using AnimationInjector::WriteOwnedDrivenFlag;
int main() {
    bool owned = false, graph = false;
    int writes = 0;
    auto write = [&](bool value) {
        ++writes;
        graph = value;
        return true;
    };
    // Repeat the reported sequence: the second exit blend reasserts the flag.
    for (int attempt = 0; attempt < 3; ++attempt) {
        assert(WriteOwnedDrivenFlag(owned, true, false, write));
        assert(owned && graph);
        assert(WriteOwnedDrivenFlag(owned, false, false, write));
        assert(owned && !graph);
        if (attempt == 1) graph = true;
        assert(WriteOwnedDrivenFlag(owned, false, true, write));
        assert(!owned && !graph);  // The next Grapple can observe both participants released.
    }
    const int before = writes;
    graph = true;  // A subsequent action owns this value; old cleanup must not change it.
    WriteOwnedDrivenFlag(owned, false, true, write);
    assert(writes == before && graph);
    WriteOwnedDrivenFlag(owned, true, false, write);
    WriteOwnedDrivenFlag(owned, false, false, write);
    graph = true;
    WriteOwnedDrivenFlag(owned, false, true, write);  // Load/forced finish also releases retained ownership.
    assert(!owned && !graph);
    WriteOwnedDrivenFlag(owned, true, false, [](bool) { return false; });
    assert(!owned);  // A failed enable must not acquire ownership.
    bool recovering = true;
    bool rotation = false;
    int rotationWrites = 0;
    auto readRotation = [&](bool& value) {
        value = rotation;
        return true;
    };
    auto writeRotation = [&](bool value) {
        rotation = value;
        ++rotationWrites;
        return true;
    };
    // Independent native selector recovered from the engine's two-flag branch.
    enum Mode { MotionDriven, AnimationDriven, AllowRotation };
    auto mode = [&] { return rotation ? AllowRotation : graph ? AnimationDriven : MotionDriven; };
    graph = false;
    rotation = true;
    assert(mode() == AllowRotation);  // Regression: clearing driven alone is insufficient.
    graph = true;
    rotation = false;
    assert(mode() == AnimationDriven);
    WriteOwnedDrivenFlag(owned, true, false, write);
    for (int frame = 0; frame < 8; ++frame) {
        graph = true;  // The outgoing FullBody graph reasserts this during blending.
        rotation = true;
        AnimationInjector::MaintainRecoveredDrivenFlag(recovering, owned, true, write);
        AnimationInjector::MaintainRecoveredRotation(recovering, readRotation, writeRotation);
        assert(recovering && owned && !graph);
        assert(!rotation && rotationWrites == frame + 1 && mode() == MotionDriven);
    }
    graph = true;  // An attack, pair or ragdoll takes ownership before deactivation.
    rotation = true;
    const int atTransfer = writes;
    AnimationInjector::MaintainRecoveredDrivenFlag(recovering, owned, false, write);
    AnimationInjector::MaintainRecoveredRotation(recovering, readRotation, writeRotation);
    assert(!recovering && !owned && graph && writes == atTransfer);
    assert(rotation && rotationWrites == 8 && mode() == AllowRotation);
    AnimationInjector::MaintainRecoveredDrivenFlag(recovering, owned, true, write);
    AnimationInjector::MaintainRecoveredRotation(recovering, readRotation, writeRotation);
    WriteOwnedDrivenFlag(owned, false, true, write);
    assert(graph && writes == atTransfer);  // Neither later frames nor Finish overwrite the new owner.
    assert(rotation && rotationWrites == 8);
    // Five recoveries: native state selection stays motion-driven throughout a
    // 0.4s pose blend, and Finish does not re-enable animation rotation.
    for (int attempt = 0; attempt < 5; ++attempt) {
        recovering = true;
        WriteOwnedDrivenFlag(owned, true, false, write);
        for (int frame = 0; frame < 40; ++frame) {
            graph = true;
            rotation = (frame % 2) == 0;
            AnimationInjector::MaintainRecoveredDrivenFlag(recovering, owned, true, write);
            AnimationInjector::MaintainRecoveredRotation(recovering, readRotation, writeRotation);
            assert(mode() == MotionDriven);
        }
        graph = true;
        rotation = true;  // Final source deactivation after the last update.
        AnimationInjector::MaintainRecoveredDrivenFlag(recovering, owned, true, write);
        AnimationInjector::MaintainRecoveredRotation(recovering, readRotation, writeRotation);
        recovering = false;
        WriteOwnedDrivenFlag(owned, false, true, write);
        AnimationInjector::MaintainRecoveredRotation(recovering, readRotation, writeRotation);
        assert(mode() == MotionDriven && !owned);
    }
    std::cout << "Driven recovery: native mode regression, 5 x 40 blend frames, transfer and final cleanup passed\n";
}
