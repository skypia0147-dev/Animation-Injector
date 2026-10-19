#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <array>
#include <vector>
#include <limits>
#include <RE/N/NiTransform.h>
#include "runtime/CarryPlacement.h"
#include "runtime/CarryHandoff.h"
#include "runtime/CarryOwnership.h"
#include "core/CarrySyncStep.h"

namespace {
void Check(bool ok, const char* message) {
    if (!ok) {
        std::cerr << "Carry placement failed: " << message << '\n';
        std::abort();
    }
}
float Distance(const RE::NiPoint3& a, const RE::NiPoint3& b) {
    const auto d = a - b;
    return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
}
RE::NiTransform Transform(RE::NiPoint3 p, RE::NiPoint3 angles, float scale = 1) {
    RE::NiTransform result;
    result.translate = p;
    result.rotate.SetEulerAnglesXYZ(angles);
    result.scale = scale;
    return result;
}
}

void TestCarryPlacement() {
    using AnimationInjector::CarryPlacement::Solve;
    float maxError = 0;
    unsigned frames = 0;
    for (float sceneScale : {0.8f, 1.0f, 1.2f}) {
        // A nonzero reference offset must remain local to the actor's 3D,
        // including when the actor is away from the world's origin.
        const RE::NiPoint3 actorInScene{2, -3, -4};
        for (unsigned frame = 0; frame < 7200; ++frame) {
            const float t = static_cast<float>(frame) / 60;
            auto carrier = Transform({20391.84f + 200 * std::sin(t), -46026.73f + 170 * std::cos(t), -144.70f},
                {0.04f * std::sin(t * 2), 0.03f * std::cos(t), t * 0.7f});
            auto shoulder = carrier * Transform({8, 12, 106}, {0, 0.1f * std::sin(t), 0});
            // Compose two local ancestors, with an authored root translation
            // that settles after the pair and then changes with the idle pose.
            auto rootInScene = Transform({0, 6, 1}, {0, 0, t * 0.01f}, 0.95f) *
                               Transform({2 * std::sin(t), 3 * std::cos(t), 70 * std::exp(-t) + 4},
                                   {0.02f * std::sin(t), 0.05f * std::cos(t), 0}, 1.02f);
            shoulder.scale = sceneScale * rootInScene.scale;
            const auto result = Solve(shoulder, rootInScene, actorInScene);
            const auto reconstructed = result.scene * rootInScene;
            const float error = Distance(reconstructed.translate, shoulder.translate);
            if (error > maxError) maxError = error;
            Check(error < 0.03f, "root-to-shoulder position");
            Check(Distance(result.scene.Invert() * result.actor, actorInScene) < 0.03f, "stable actor-in-scene offset");
            Check(Distance(result.actor, carrier.translate) < 220, "bounded actor placement");
            Check(std::abs(result.scene.scale - sceneScale) < 0.00001f, "scale preservation");
            for (unsigned r = 0; r < 3; ++r)
                for (unsigned c = 0; c < 3; ++c)
                    Check(std::abs(reconstructed.rotate.entry[r][c] - shoulder.rotate.entry[r][c]) < 0.00001f,
                        "root-to-shoulder rotation");
            ++frames;
        }
    }

    // Deliberately delay the scene cache while the reference updates promptly.
    // This is a regression model, not an assertion of Skyrim's exact cadence.
    const auto anchor = Transform({0, 0, 100}, {0, 0, 0});
    const auto localRoot = Transform({0, 0, 70}, {0, 0, 0});
    RE::NiTransform cachedScene;
    RE::NiPoint3 oldActor{};
    float oldMaxError = 0;
    for (unsigned frame = 0; frame < 32; ++frame) {
        const auto cachedRoot = cachedScene * localRoot;
        const auto oldRelative = cachedScene.Invert() * cachedRoot;
        const auto oldDesired = anchor * oldRelative.Invert();
        oldActor = oldActor + oldDesired.translate - cachedScene.translate;
        const auto result = Solve(anchor, localRoot, RE::NiPoint3{});
        Check(Distance(result.actor, {0, 0, 30}) < 0.00001f, "delayed cache must not feed back into placement");
        const float error = Distance(oldActor, result.actor);
        if (error > oldMaxError) oldMaxError = error;
        if (frame % 8 == 7) cachedScene.translate = oldActor;
    }
    Check(oldMaxError > 220, "regression must expose the previous accumulation");
    std::cout << "Production CarryPlacement/NiTransform: " << frames
              << " moving/rotating/scaled frames and 32 delayed-cache frames passed; max root error=" << maxError
              << "; previous-formula drift=" << oldMaxError << '\n';
}

void TestNativeCarryPlacement() {
    using namespace AnimationInjector::CarryPlacement;
    unsigned frames = 0;
    for (float actorScale : {0.8f, 1.0f, 1.5f})
        for (float initialAnchorScale : {0.7f, 1.0f, 2.0f}) {
            const auto initialAnchor = Transform({20400, -45800, 100}, {0.1f, -0.3f, 1.7f}, initialAnchorScale);
            const auto rootInScene = Transform({3, 7, 60}, {0.03f, 0.02f, 0.6f}, 0.9f);
            auto desiredRoot = Transform(initialAnchor.translate, {0, 0, 2.0f}, actorScale * rootInScene.scale);
            const auto desiredScene = Solve(desiredRoot, rootInScene, RE::NiPoint3{}).scene;
            const auto offset = NativeOffset(initialAnchor, desiredScene);
            const auto initialScene = NativeScene(initialAnchor, offset, actorScale);
            Check(Distance((initialScene * rootInScene).translate, initialAnchor.translate) < 0.03f,
                "native offset compensates authored root at attachment");
            for (unsigned frame = 0; frame < 1200; ++frame) {
                const float t = frame / 60.0f;
                const auto anchor =
                    Transform({20400 + 170 * std::sin(t), -45800 + 200 * std::cos(t), 100 + 3 * std::sin(3 * t)},
                        {0.04f * std::sin(t), 0.08f * std::cos(t), t * 0.7f}, 0.5f + 0.2f * t);
                const auto scene = NativeScene(anchor, offset, actorScale);
                // Independent native contract: R*offset + translation, no anchor
                // scale; keep the actor's existing scene scale, including root scale.
                const auto expected = anchor.rotate * offset.translate + anchor.translate;
                Check(Distance(scene.translate, expected) < 0.03f, "native position contract");
                Check(std::abs(scene.scale - actorScale) < 0.00001f, "native follower scale stays unchanged");
                const auto relative = anchor.rotate.Transpose() * (scene.translate - anchor.translate);
                Check(Distance(relative, offset.translate) < 0.03f,
                    "attachment does not accumulate actor/cache feedback");
                auto unitAnchor = anchor;
                unitAnchor.scale = 1;
                Check(Distance(NativeScene(unitAnchor, offset, actorScale).translate, scene.translate) < 0.00001f,
                    "anchor scale is ignored by native sync");
                ++frames;
            }
        }
    std::cout << "Native mount offset: " << frames << " moving/rotating/scaled frames passed\n";
}

void TestCarryRelativeHandoff() {
    using namespace AnimationInjector::CarryPlacement;
    using Pose = AnimationInjector::CarryHandoff::Pose<RE::NiTransform>;
    Pose missing;
    Check(!missing.Freeze(), "an exit without a playing sample cannot attach");
    missing.Sample(Transform({0, 0, 0}, {0, 0, 0}), Transform({1, 2, 3}, {0, 0, 0}));
    Check(!missing.valid, "a late idle sample cannot revive a missing ending");
    unsigned frames = 0;
    float maxError = 0;
    for (float actorScale : {0.8f, 1.0f, 1.5f})
        for (float anchorScale : {0.7f, 1.0f, 2.0f}) {
            auto anchor = Transform({20400, -45800, 130}, {0.2f, -0.3f, 1.7f}, anchorScale);
            auto unitAnchor = anchor;
            unitAnchor.scale = 1;
            const auto relation = Transform({10, 25, 5}, {0.3f, 0.1f, 0.52f}, actorScale);
            const auto authoredRoot = unitAnchor * relation;
            Pose snapshot;
            snapshot.Sample(anchor, Transform({20380, -45790, 20}, {0, 0, 0}));
            snapshot.Sample(anchor, authoredRoot);
            Check(snapshot.Freeze(), "last playing root is available at exit");
            // Deliberately wrong idle pose must not overwrite either translation
            // or rotation. The source animation's root resets during the blend.
            snapshot.Sample(anchor, Transform({0, 0, 0}, {0, 0, 0}));
            Check(snapshot.samples == 2, "exit freeze rejects subsequent samples");
            for (unsigned frame = 0; frame < 120; ++frame) {
                const float blend = frame / 119.0f;
                const auto movingAnchor = Transform(anchor.translate + RE::NiPoint3{blend * 30, blend * 20, blend * 2},
                    {0.2f - blend * 0.1f, -0.3f, 1.7f + blend * 0.4f}, anchorScale + blend);
                auto currentRootLocal = Transform({12 * (1 - blend), -15 * (1 - blend), 70 * (1 - blend)},
                    {0.3f * (1 - blend), 0.1f * (1 - blend), 0.52f * (1 - blend)});
                const auto desiredRoot = snapshot.Root(movingAnchor, actorScale * currentRootLocal.scale);
                const auto scene = Solve(desiredRoot, currentRootLocal, RE::NiPoint3{}).scene;
                const auto actualRoot = scene * currentRootLocal;
                auto referenceAnchor = movingAnchor;
                referenceAnchor.scale = 1;
                const auto expectedRoot = referenceAnchor * relation;
                const float error = Distance(actualRoot.translate, expectedRoot.translate);
                maxError = std::max(maxError, error);
                Check(error < 0.03f, "exit blend must preserve nonzero authored relative root position");
                Check(Distance(actualRoot.translate, movingAnchor.translate) > 20, "no snap to the anchor origin");
                for (unsigned r = 0; r < 3; ++r)
                    for (unsigned c = 0; c < 3; ++c)
                        Check(std::abs(actualRoot.rotate.entry[r][c] - expectedRoot.rotate.entry[r][c]) < 0.00001f,
                            "full relative rotation survives pose reset");
                Check(std::abs(scene.scale - actorScale) < 0.00001f, "handoff preserves target scale");
                // Native attachment inherits precisely the same scene as the
                // temporary visual hold, even when the local root has reset.
                const auto native = NativeScene(movingAnchor, NativeOffset(movingAnchor, scene), actorScale);
                Check(Distance((native * currentRootLocal).translate, actualRoot.translate) < 0.03f,
                    "visual-to-native handoff has no position jump");
                for (unsigned r = 0; r < 3; ++r)
                    for (unsigned c = 0; c < 3; ++c)
                        Check(std::abs(native.rotate.entry[r][c] - scene.rotate.entry[r][c]) < 0.00001f,
                            "visual-to-native handoff has no rotation jump");
                ++frames;
            }
        }
    std::cout << "Authored relative carry handoff: " << frames
              << " translated/rotated/scaled exit frames and missing/late-pose guards passed; max error=" << maxError
              << '\n';
    // Movement may finish the player's exit blend immediately while the
    // partner still retires. Preserve the sampled relation across this gap,
    // including a root reset in one frame instead of a gradual blend.
    for (unsigned partnerDelay : {0u, 1u, 4u, 12u}) {
        const auto anchor = Transform({700, -800, 150}, {0, 0, .7f});
        const auto relation = Transform({14, 27, -8}, {.2f, -.3f, .4f});
        Pose snapshot;
        snapshot.Sample(anchor, anchor * relation);
        Check(snapshot.Freeze(), "movement exit retains the last lifting pose");
        unsigned attaches = 0;
        for (unsigned frame = 0; frame <= partnerDelay; ++frame) {
            auto moving =
                Transform(anchor.translate + RE::NiPoint3{float(frame) * 8, 6, 0}, {0, 0, .7f + float(frame) * .08f});
            const auto resetRoot = Transform({0, 0, 0}, {0, 0, 0});
            snapshot.Sample(moving, resetRoot);  // Idle must not replace the carry pose.
            const auto desired = snapshot.Root(moving, 1.f);
            const auto heldScene = Solve(desired, resetRoot, RE::NiPoint3{}).scene;
            Check(Distance((heldScene * resetRoot).translate, (moving * relation).translate) < .03f,
                "moving during partner exit must keep the target at the live anchor");
            const bool partnerDone = frame == partnerDelay;
            const auto outcome = AnimationInjector::CarryOwnership::Completed(
                true, partnerDone, true, true, true, true, partnerDone, true, partnerDone);
            Check(outcome == (partnerDone ? AnimationInjector::CarryOwnership::PairResult::kSuccess
                                          : AnimationInjector::CarryOwnership::PairResult::kWait),
                "attach waits for both participants after movement cancel");
            if (outcome == AnimationInjector::CarryOwnership::PairResult::kSuccess) {
                ++attaches;
                const auto attached = NativeScene(moving, NativeOffset(moving, heldScene), 1.f);
                Check(Distance(attached.translate, heldScene.translate) < .03f,
                    "accelerated exit to attachment has no positional jump");
                for (unsigned r = 0; r < 3; ++r)
                    for (unsigned c = 0; c < 3; ++c)
                        Check(std::abs(attached.rotate.entry[r][c] - heldScene.rotate.entry[r][c]) < .00001f,
                            "accelerated exit to attachment preserves orientation");
            }
        }
        Check(attaches == 1 && snapshot.samples == 1, "one attachment and no post-exit pose replacement");
    }
    std::cout
        << "Movement-shortened pickup exit: instant root reset, moving/turning anchor and 0/1/4/12-frame partner exit delay passed\n";
}

namespace {
struct UpdateData {
    enum Flag : unsigned { kNone = 0, kDirty = 1, kDisableCollision = 1 << 13 };
    unsigned flags = 0;
    float time = 0;
};
struct SceneNode;
struct SyncController {
    SceneNode* target = nullptr;
    SyncController* next = nullptr;
    SyncController* GetNext() { return next; }
};
struct SceneNode {
    SceneNode* parent = nullptr;
    RE::NiTransform local, world;
    std::vector<SceneNode*> children;
    unsigned controllerTicks = 0, collisionTicks = 0, refreshes = 0;
    SyncController* controllers = nullptr;
    SyncController* GetControllers() { return controllers; }
    virtual ~SceneNode() = default;
    void Refresh() {
        ++refreshes;
        world = parent ? parent->world * local : local;
    }
    // Model the two native virtual routes inspected in AE 1.6.1170.
    virtual void UpdateTransformAndBounds(UpdateData& data) {
        Refresh();
        for (auto* child : children) child->UpdateTransformAndBounds(data);
    }
    virtual void UpdateDownwardPass(UpdateData& data, unsigned flags) {
        if (data.flags & 1) ++controllerTicks;
        if (!(data.flags & (1 << 13))) ++collisionTicks;
        Refresh();
        for (auto* child : children) child->UpdateDownwardPass(data, flags);
    }
};
struct FlatTree : SceneNode {
    struct Bone {
        RE::NiTransform local, world;
        int parent = -1;
        SceneNode* node = nullptr;
    };
    std::array<Bone, 3> bones;
    void UpdateDownwardPass(UpdateData& data, unsigned flags) override {
        SceneNode::UpdateDownwardPass(data, flags);
        for (auto& bone : bones)
            bone.world =
                bone.node ? bone.node->world : (bone.parent < 0 ? world : bones[bone.parent].world) * bone.local;
    }
};
}

void TestCarrySceneRefresh() {
    SceneNode room, scene, root;
    FlatTree tree;
    room.world = Transform({20000, -45000, -140}, {0, 0, 0.3f});
    scene.parent = &room;
    scene.children = {&tree};
    tree.parent = &scene;
    tree.children = {&root};
    root.parent = &tree;
    tree.bones[0].node = &root;
    tree.bones[1].parent = 0;
    tree.bones[1].local.translate = {0, 0, -12};
    tree.bones[2].parent = 1;
    tree.bones[2].local.translate = {6, 0, -14};
    UpdateData data;
    data.flags = UpdateData::Flag::kDisableCollision;
    scene.UpdateDownwardPass(data, 0);

    const auto raised = Transform({20400, -45800, -30}, {0, 0, 0.7f});
    scene.local = room.world.Invert() * raised;
    scene.UpdateTransformAndBounds(data);
    const auto toeFromPose = raised * tree.bones[1].local * tree.bones[2].local;
    Check(Distance(root.world.translate, raised.translate) < 0.03f, "old root-only check appears correct");
    Check(Distance(tree.bones[2].world.translate, toeFromPose.translate) > 100,
        "old pass must leave the flattened foot behind despite a correct root");

    for (unsigned frame = 0; frame < 600; ++frame) {
        const float t = static_cast<float>(frame) / 60;
        const auto desired =
            Transform({20400 + 30 * std::sin(t), -45800 + 50 * std::cos(t), -30}, {0, 0.1f * std::sin(t), t});
        AnimationInjector::CarryPlacement::ApplyScene(scene, desired, data);
        const auto expectedToe = desired * tree.bones[1].local * tree.bones[2].local;
        Check(Distance(root.world.translate, desired.translate) < 0.03f, "materialized root follows");
        Check(Distance(tree.bones[0].world.translate, root.world.translate) < 0.03f, "mapped skin bone follows");
        Check(Distance(tree.bones[2].world.translate, expectedToe.translate) < 0.03f, "flattened foot follows");
    }
    for (auto* node : {&room, &scene, static_cast<SceneNode*>(&tree), &root}) {
        Check(!node->controllerTicks, "transform refresh must not advance controllers");
        Check(!node->collisionTicks, "transform refresh must not move collision objects");
    }
    std::cout << "Carry ApplyScene: stale flattened feet reproduced; 600 full skeleton refresh frames passed\n";
}

void TestNativeCarryTick() {
    using AnimationInjector::CarrySyncStep::Advance;
    SceneNode scene, root;
    FlatTree skin;
    scene.children = {&skin};
    skin.parent = &scene;
    skin.children = {&root};
    root.parent = &skin;
    skin.bones[0].node = &root;
    skin.bones[1].parent = 0;
    skin.bones[1].local.translate = {0, 0, -12};
    skin.bones[2].parent = 1;
    skin.bones[2].local.translate = {5, 0, -14};
    SyncController other{&scene}, owned{&scene};
    other.next = &owned;
    scene.controllers = &other;
    RE::NiPoint3 actor{};
    const auto original = actor;
    auto desired = Transform({20482, -46015, -30}, {0, 0, 0.8f});
    unsigned nativeTicks = 0;
    const auto native = [&](SyncController* controller, UpdateData& data) {
        Check(controller == &owned, "only the owned native sync is dispatched");
        Check(data.time == 0 && data.flags == 0, "native tick starts with independent update data");
        // Model the inspected REFRSyncController's scene-local/ref writes.
        // No normal actor processing runs while the AI is disabled.
        scene.local = desired;
        actor = desired.translate;
        ++nativeTicks;
        data.flags = UpdateData::Flag::kDirty;
        data.time = 100;
    };
    // Registration alone leaves an AI-paused actor and its skin at the origin.
    Check(Distance(actor, original) == 0 && Distance(actor, desired.translate) > 100,
        "reproduce registered-but-not-updated target");
    for (unsigned frame = 0; frame < 600; ++frame) {
        const float t = frame / 60.0f;
        desired =
            Transform({20482 + 100 * std::sin(t), -46015 + 70 * std::cos(t), -30}, {0, 0.03f * std::sin(t), 0.8f + t});
        Check(Advance<UpdateData>(&scene, &owned, native), "explicit update succeeds while AI is paused");
        Check(Distance(actor, desired.translate) < 0.001f, "reference follows after explicit native tick");
        Check(Distance(root.world.translate, desired.translate) < 0.001f, "skeleton refresh occurs after native tick");
        Check(Distance(skin.bones[2].world.translate, (desired * skin.bones[1].local * skin.bones[2].local).translate) <
                  0.03f,
            "unmapped foot follows without stretching");
        for (auto* node : {&scene, static_cast<SceneNode*>(&skin), &root}) {
            Check(!node->controllerTicks, "scene pass does not advance unrelated controllers or tick native twice");
            Check(!node->collisionTicks, "scene pass does not synchronize collision bodies");
            Check(node->refreshes == frame + 1, "one full transform refresh per explicit tick");
        }
    }
    Check(nativeTicks == 600, "one native tick per frame");
    Check(!Advance<UpdateData>(static_cast<SceneNode*>(nullptr), &owned, native), "missing scene rejected");
    Check(!Advance<UpdateData>(&scene, static_cast<SyncController*>(nullptr), native), "missing controller rejected");
    SceneNode replacement;
    owned.target = &replacement;
    Check(!Advance<UpdateData>(&scene, &owned, native), "retargeted controller rejected");
    owned.target = &scene;
    other.next = &other;
    Check(!Advance<UpdateData>(&scene, &owned, native),
        "unlinked controller and cyclic foreign list rejected with a bound");
    other.next = &owned;
    Check(!Advance<UpdateData>(&scene, &owned,
              [&](auto* controller, auto&) {
                  Check(controller == &owned, "detach callback belongs to ours");
                  controller->target = nullptr;
              }),
        "detachment during update prevents stale scene refresh");
    owned.target = &scene;
    Check(!Advance<UpdateData>(&scene, &owned, [&](auto*, auto&) { other.next = nullptr; }),
        "replacement during update prevents stale scene refresh");
    Check(!Advance<UpdateData>(&scene, &owned, native), "old controller cannot run after replacement");
    Check(scene.refreshes == 600 && nativeTicks == 600, "rejected paths never tick or refresh after release");
    std::cout
        << "Native sync step: paused-AI freeze reproduced; 600 owned ticks/full-skin frames and 7 ownership/exit guards passed\n";
}
