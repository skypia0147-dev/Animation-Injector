#pragma once

namespace AnimationInjector::CarrySyncStep {
// Check exact identity; never advance another controller of the same type.
template<class Scene, class Controller>
bool Linked(Scene* scene, Controller* owned) {
    if (!scene || !owned || owned->target != scene) return false;
    unsigned budget = 256;
    for (auto* current = scene->GetControllers(); current && budget--; current = current->GetNext())
        if (current == owned) return true;
    return false;
}

// AI-disabled actors cannot be assumed to run their scene controllers. Tick
// only our native reference sync, then propagate the resulting local transform
// through the entire skeleton. The second pass must not tick other controllers
// or synchronize skeleton collision bodies. CarryPhysics separately relocates
// the carried controller with contacts disabled.
template<class UpdateData, class Scene, class Controller, class NativeUpdate>
bool Advance(Scene* scene, Controller* owned, NativeUpdate update) {
    if (!Linked(scene, owned)) return false;
    UpdateData syncData{};
    update(owned, syncData);
    if (!Linked(scene, owned)) return false;
    UpdateData transformData{};
    transformData.flags = UpdateData::Flag::kDisableCollision;
    scene->UpdateDownwardPass(transformData, 0);
    return true;
}
}
