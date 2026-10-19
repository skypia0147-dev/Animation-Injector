#include "runtime/PairStartBlend.h"
#include "runtime/PairStartCamera.h"
#include "core/NativePairStartPose.h"
#include "core/NativePairObservation.h"
#include "core/NativePairSpace.h"
#include <iostream>
#include <cstdlib>
#include <limits>

namespace {
void Check(bool result, const char* message) {
    if (!result) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
bool Near(const RE::NiTransform& a, const RE::NiTransform& b) {
    if ((a.translate - b.translate).Length() > .005f || std::abs(a.scale - b.scale) > .0001f) return false;
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j)
            if (std::abs(a.rotate.entry[i][j] - b.rotate.entry[i][j]) > .0001f) return false;
    return true;
}
RE::NiTransform Pose(float x, float y, float z, float yaw, float scale = 1) {
    RE::NiTransform p;
    p.translate = {x, y, z};
    p.rotate.SetEulerAnglesXYZ({0, 0, yaw});
    p.scale = scale;
    return p;
}
}
int main() {
    using namespace AnimationInjector;
    {
        using NativePairSpace::Snapshot;
        using NativePairSpace::Compatible;
        // Report #1 crosses X=20480 during placement; #8/#9 begin on
        // opposite sides. Cell IDs below represent adjacent loaded cells.
        const Snapshot west{{101, 60, false}, 77}, east{{102, 60, false}, 77};
        Check(Compatible(west, east) && Compatible(east, west), "adjacent exterior actors can start a pair");
        PairStartBlend::Clock crossing;
        crossing.Begin(.4f);
        float lastWeight = 0;
        for (unsigned frame = 0; frame <= 40; ++frame) {
            const auto live = frame < 2 ? west : east;
            Check(Compatible(west, live), "crossing at 0.02s must retain the captured start visual");
            const auto weight = crossing.Weight(true, frame * .01f, frame * .01f);
            Check(weight >= lastWeight, "cell crossing cannot restart the shared blend clock");
            lastWeight = weight;
        }
        Check(lastWeight == 1, "cell crossing reaches the full 0.4s blend");
        Check(!Compatible(west, Snapshot{{102, 61, false}, 77}), "different exterior world is not continuous");
        Check(!Compatible(west, Snapshot{{102, 60, true}, 77}), "interior transition still stops the blend");
        Check(!Compatible(west, Snapshot{{102, 60, false}, 78}), "physics world replacement still stops the blend");
        Check(!Compatible(west, Snapshot{{102, 60, false}, 0}), "unloaded physics world is unavailable");
        Check(!Compatible(west, Snapshot{{0, 60, false}, 77}), "missing cell is unavailable");
        const Snapshot room{{201, 0, true}, 88};
        Check(Compatible(room, room), "same loaded interior remains supported");
        Check(!Compatible(room, Snapshot{{202, 0, true}, 88}), "different interiors are never merged");
        Check(!Compatible(Snapshot{{101, 0, false}, 77}, Snapshot{{102, 0, false}, 77}),
            "unknown exterior world cannot join cells");
        std::cout
            << "Native pair space: adjacent-cell start, 41 crossing frames and world/interior/physics exit guards passed\n";
    }
    // A reused node reports the old clip's end before the new scene activates.
    // Neither that clock nor its delayed deactivate may end the new blend.
    for (unsigned repeat = 0; repeat < 100; ++repeat) {
        NativePairObservation::Clip self, target;
        PairStartBlend::Clock start;
        start.Begin(.4f);
        constexpr std::uintptr_t scene = 42, oldScene = 41;
        self.Observe(false, scene, scene, false, 4);
        target.Observe(false, scene, scene, true, 4);
        self.Observe(true, scene, oldScene, true, 4);
        target.Observe(true, scene, oldScene, false, 4);
        Check(!self.ended && !target.ended && self.time < 0 && target.time < 0,
            "old scene cannot own new startup clock or exit");
        for (unsigned frame = 0; frame <= 40; ++frame) {
            const float time = frame * .01f;
            self.Observe(true, scene, scene, false, time);
            target.Observe(true, scene, scene, false, time);
            const float weight = start.Weight(true, self.time, target.time);
            Check(frame == 40 ? weight == 1 : weight < 1, "repeated pair keeps the entire new start blend");
        }
        self.Observe(true, scene, scene, true, 4);
        Check(self.ended, "actual current-scene cancellation still closes startup");
    }
    // Reproduce the second native alignment using a stale graph origin.
    // Rebased generator caches must carry the requested world pose exactly,
    // while retaining the engine's scale and leaving authored marks alone.
    for (unsigned i = 0; i < 120; ++i) {
        const float yaw = -3.14f + i * .053f;
        const PairAlignmentMath::Pose goal{200 + i * 1.f, -400 + i * 2.f, -144, yaw};
        RE::hkQsTransform old{};
        old.translation = {-900, 1800, 300, 0};
        old.rotation.vec = {0, 0, 0, 1};
        old.scale = {.8f, 1.2f, 1.5f, 0};
        const auto rebased = NativePairStartPose::Rebase(old, goal);
        Check(rebased.has_value(), "valid activation cache accepts planned pose");
        PairPlacementMath::Mark mark;
        for (unsigned j = 0; j < 3; ++j) {
            mark.translation[j] = rebased->translation.quad.m128_f32[j];
            Check(rebased->scale.quad.m128_f32[j] == old.scale.quad.m128_f32[j],
                "native scale survives activation rebasing");
        }
        for (unsigned j = 0; j < 4; ++j) mark.rotation[j] = rebased->rotation.vec.quad.m128_f32[j];
        const auto forward = PairPlacementMath::Rotate(mark.rotation, {0, 1, 0});
        Check(std::abs(PairAlignmentMath::Wrap(std::atan2(forward[0], forward[1]) - yaw)) < .0001f,
            "native quaternion uses actor yaw convention, not reversed rotation");
        Check(mark.translation[0] == goal.x && mark.translation[1] == goal.y && mark.translation[2] == goal.z,
            "second native alignment cannot reuse old graph position");
        Check(old.translation.quad.m128_f32[0] == -900, "input graph snapshot remains read-only");
        old.rotation.vec = {0, 0, 0, 0};
        Check(!NativePairStartPose::Rebase(old, goal), "uninitialized native cache must not be patched");
    }
    PairStartBlend::Clock clock;
    clock.Begin(.4f);
    for (unsigned i = 0; i < 100; ++i)
        Check(clock.Weight(false, 1, 1) == 0, "pending/native preparation must not consume blend");
    Check(clock.Weight(true, .2f, -1) == 0, "wait for both actual clip clocks");
    Check(clock.Weight(true, .2f, .1f) > .15f && clock.Value() < .16f, "both roles use slower observed clip clock");
    const auto previous = clock.Value();
    Check(clock.Weight(true, .01f, .01f) == previous, "stale observation must not reverse blend");
    Check(clock.Weight(true, .2f, .2f) == .5f, "enter-blend midpoint; no added settle delay");
    Check(clock.Weight(true, .4f, .39f) < 1, "fast participant cannot end partner's blend early");
    Check(clock.Weight(true, .4f, .4f) == 1, "exact enter duration ends both roles together");
    clock.Begin(0);
    Check(clock.Weight(false, 0, 0) == 0 && clock.Weight(true, 0, 0) == 1,
        "zero duration still waits for actual playback");
    clock.Begin(.3f);
    Check(clock.Weight(true, std::numeric_limits<float>::quiet_NaN(), .1f) == 0,
        "invalid clock cannot release held visual");

    // Model/parent can jump and rotate during native preparation. The visible
    // root at weight zero must remain at its pre-start world pose for both roles.
    for (unsigned i = 0; i < 120; ++i)
        for (unsigned role = 0; role < 2; ++role) {
            const float turn = -3.14f + i * .053f;
            const auto held = Pose(120 + role * 80, 200, role ? 160.f : 3.f, -turn, .8f + role * .4f);
            const auto parent = Pose(170 + role * 80, 222, 0, turn, 1.2f);
            const auto local = Pose(0, 0, role ? 100.f : 0.f, turn * .5f, .9f);
            const auto motion = parent * local;
            const auto start = PairStartBlend::Local(parent, held, motion, 0);
            const auto end = PairStartBlend::Local(parent, held, motion, 1);
            Check(Near(parent * start, held), "native mark placement must not teleport visible root");
            Check(Near(end, local), "completed blend must exactly return to authored local pose");
            const auto midway = parent * PairStartBlend::Local(parent, held, motion, .5f);
            Check((midway.translate - (held.translate + motion.translate) * .5f).Length() < .005f,
                "world-space halfway blend");
            // Simulate a descendant skin bone: parent/root correction is composed once.
            const auto foot = Pose(3, 5, -10, .1f);
            Check(Near(parent * start * foot, held * foot), "unmapped descendant follows root without stretching");
        }
    // Native acceptance owns travel limits. Finite large transitions must still
    // retain their first frame and finish at the authored pose without snapping.
    for (float distance : {401.f, 800.f, 5000.f}) {
        const auto parent = Pose(distance, -distance, 90, 1.7f);
        const auto held = Pose(0, 0, 170, -1.1f);
        const auto local = Pose(0, 5, 0, .2f);
        Check(Near(parent * PairStartBlend::Local(parent, held, parent * local, 0), held),
            "native-approved long transition preserves the initial visible pose");
        Check(Near(PairStartBlend::Local(parent, held, parent * local, 1), local),
            "native-approved long transition reaches the authored pose");
    }
    // Putdown hands the carried world pose to the common native blend. Its
    // physical parent may already be on the ground in a newly loaded cell.
    for (unsigned i = 0; i < 120; ++i) {
        const auto held = Pose(200 + i * 3.f, -500, 160, .037f * i, 1.2f);
        const auto oldParent = Pose(-200, 100, 0, -.021f * i);
        const auto groundParent = Pose(220 + i * 3.f, -505, -12, .053f * i, .9f);
        const auto authored = Pose(1, 5, 140, -.02f * i);
        auto local = authored;
        PairStartBlend::Layer<RE::NiTransform> carryLayer, nativeLayer;
        carryLayer.Apply(local, oldParent.Invert() * held);
        Check(Near(oldParent * local, held), "detachment bridge holds shoulder pose before native Start");
        Check(carryLayer.Restore(local, Near), "handoff removes exactly the old carry overlay");
        const auto motion = groundParent * local;
        PairStartBlend::Clock common;
        common.Begin(.4f);
        const auto weight = common.Weight(false, -1, -1);
        nativeLayer.Apply(local, PairStartBlend::Local(groundParent, held, motion, weight));
        Check(Near(groundParent * local, held), "handoff cannot flash the already grounded physical base");
        Check(!carryLayer.Restore(local, Near) && Near(groundParent * local, held),
            "old carry cleanup cannot undo native visible pose");
        Check(nativeLayer.Restore(local, Near) && Near(local, authored),
            "common update restores authored pose before next engine frame");
        nativeLayer.Apply(
            local, PairStartBlend::Local(groundParent, held, groundParent * authored, common.Weight(true, .4f, .4f)));
        Check(Near(local, authored), "common clip clock alone completes putdown transition");
        const auto ragdoll = Pose(4, 9, 13, 1.5f);
        local = ragdoll;
        Check(!nativeLayer.Restore(local, Near) && Near(local, ragdoll),
            "new physics pose wins over stale native cleanup");
    }
    struct Camera {
        bool toggleAnimCam = false, freeRotationEnabled = true;
        float orbit = 2.81223f, body = 3.20292f;
    } camera;
    unsigned calls = 0;
    const auto original = [&] {
        ++calls;
        camera.body += camera.orbit;
        camera.orbit = 0;
        camera.freeRotationEnabled = false;
    };
    for (unsigned i = 0; i < 60; ++i) PairStartCamera::SetMode(camera, true, original);
    Check(calls == 0 && camera.body == 3.20292f && camera.orbit == 2.81223f,
        "reported 161-degree camera-to-body reset must be prevented");
    camera.orbit += .4f;
    PairStartCamera::SetMode(camera, true, original);
    Check(camera.orbit > 3.2f && camera.body == 3.20292f, "live camera orbit remains available while paired");
    camera.toggleAnimCam = true;
    PairStartCamera::SetMode(camera, true, original);
    Check(calls == 1, "authored animated-camera mode keeps original behavior");
    camera.toggleAnimCam = false;
    PairStartCamera::SetMode(camera, false, original);
    Check(calls == 2, "non-owned/finished pair passes through to existing camera hook");
    std::cout
        << "Start blend: 120 native activation cache regressions, 240 paired-role transforms, shared clip clocks, skin continuity and camera orbit regression passed\n";
}
