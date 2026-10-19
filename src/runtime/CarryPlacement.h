#pragma once

namespace AnimationInjector::CarryPlacement {
// REFRSyncController ignores the anchor's scale and leaves the follower's
// scene scale unchanged. Capture the relative offset only once, like a saddle.
template<class Transform>
Transform NativeOffset(Transform anchor, const Transform& desiredScene) {
    anchor.scale = 1;
    return anchor.Invert() * desiredScene;
}
template<class Transform>
Transform NativeScene(Transform anchor, const Transform& offset, float sceneScale) {
    anchor.scale = 1;
    auto result = anchor * offset;
    result.scale = sceneScale;
    return result;
}
// Compute each frame from a stable actor-in-scene reference captured on attach.
// Never add current actor minus cached scene-world: those can be from different
// engine update phases and feed yesterday's correction back into today's result.
template<class Transform, class Point>
auto Solve(const Transform& desiredRoot, const Transform& rootInScene, const Point& actorInScene) {
    struct Result {
        Transform scene;
        Point actor;
    };
    const auto scene = desiredRoot * rootInScene.Invert();
    return Result{scene, scene * actorInScene};
}

template<class Scene, class Transform, class UpdateData>
void ApplyScene(Scene& scene, const Transform& desiredScene, UpdateData& data) {
    scene.local = scene.parent ? scene.parent->world.Invert() * desiredScene : desiredScene;
    // The virtual downward pass refreshes BSFlattenedBoneTree's world array.
    // UpdateTransformAndBounds only walks materialized NiAVObject children and
    // leaves unmapped skin bones at the previous world position.
    scene.UpdateDownwardPass(data, 0);
}
}
