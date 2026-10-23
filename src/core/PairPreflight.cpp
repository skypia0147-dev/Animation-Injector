#include "PCH.h"
#include "core/PairPreflight.h"
#include "core/PairPlacementMath.h"
#include "runtime/PairAlignmentMath.h"
#include "core/AnimationBindingWithTriggers.h"
#include "core/NodeFactory.h"
#include "runtime/ActionRegistry.h"

namespace AnimationInjector::PairPreflight {
namespace {
std::optional<PairPlacementMath::Mark> ReadMark(
    RE::Actor* actor, const char* file, const char* prefix, PairedRig::Kind rig) {
    RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
    if (!actor->GetAnimationGraphManager(manager) || !manager) return std::nullopt;
    RE::BSSpinLockGuard lock(manager->GetRuntimeData().updateLock);
    std::optional<PairPlacementMath::Mark> result;
    bool found = false;
    for (const auto& graph : manager->graphs) {
        if (!graph) continue;
        auto& character = graph->characterInstance;
        auto* setup = character.setup.get();
        auto* data = setup ? setup->data.get() : nullptr;
        auto* strings = data ? data->stringData.get() : nullptr;
        const auto* project = strings ? strings->name.c_str() : nullptr;
        if (!project || PairedRig::CharacterName(project) != rig) continue;
        if (found) return std::nullopt;
        found = true;
        auto* set = static_cast<RE::hkbAnimationBindingSet*>(character.animationBindingSet.get());
        if (!set && setup) set = setup->animationBindingSet.get();
        const int index = NodeFactory::FindAnimationName(strings->animationNames, file);
        if (!set || !set->bindings._data || index < 0 || index >= set->bindings._size) return std::nullopt;
        auto* slot = set->bindings._data[index];
        if (!NodeFactory::SameVTable(slot, RE::VTABLE_hkbAnimationBindingWithTriggers)) return std::nullopt;
        auto* binding = slot->binding.get();
        auto* animation = binding ? binding->animation.get() : nullptr;
        if (!animation || !std::isfinite(animation->duration) || animation->duration <= 0 ||
            animation->numberOfTransformTracks <= 0 || animation->numberOfTransformTracks > 4096 ||
            !animation->annotationTracks._data || animation->annotationTracks._size <= 0 ||
            animation->annotationTracks._size > 4096)
            return std::nullopt;
        int track = -1;
        for (int i = 0; i < animation->annotationTracks._size; ++i) {
            const auto* name = animation->annotationTracks._data[i].trackName.c_str();
            if (name && std::strcmp(name, prefix) == 0) {
                track = i;
                break;
            }
        }
        if (track < 0 || track >= animation->numberOfTransformTracks) return std::nullopt;
        const auto selected = static_cast<std::uint16_t>(track);
        RE::hkQsTransform sampled{};
        // Same virtual method, sample time, and annotation-track index as GetMark.
        animation->SampleIndividualTransformTracks(0.0f, &selected, 1, &sampled);
        PairPlacementMath::Mark mark;
        for (int i = 0; i < 3; ++i) mark.translation[i] = sampled.translation.quad.m128_f32[i];
        for (int i = 0; i < 4; ++i) mark.rotation[i] = sampled.rotation.vec.quad.m128_f32[i];
        if (!PairPlacementMath::Valid(mark)) return std::nullopt;
        result = mark;
    }
    return result;
}
}
std::optional<std::array<PairPlacementMath::Mark, 2>> ReadStartMarks(
    RE::ActorHandle initiator, RE::ActorHandle target, int action) {
    if (action < 0 || action >= Actions::Count()) return std::nullopt;
    const auto& spec = Actions::Spec(action);
    if (!spec.paired.enabled || spec.clips.size() != 2 || spec.paired.applyMotionFromRoot) return std::nullopt;
    auto self = initiator.get(), other = target.get();
    if (!self || !other || self.get() == other.get()) return std::nullopt;
    for (auto* actor : {self.get(), other.get()}) {
        if (!actor->Is3DLoaded() || actor->IsDead() || actor->IsInRagdollState() || !actor->GetCharController())
            return std::nullopt;
        const auto* state = actor->AsActorState();
        if (!state || state->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal) return std::nullopt;
    }
    // GraphInjector validates the front-feed template: role 0 is the native
    // leader (target, prefix 2_), role 1 is support (initiator, null -> NPC).
    const auto targetFile = PairedRig::ClipPath(spec.clips[0].file, spec.paired.targetRig);
    const auto leader = ReadMark(other.get(), targetFile.c_str(), "2_", spec.paired.targetRig);
    const auto support = ReadMark(self.get(), spec.clips[1].file.c_str(), "NPC", PairedRig::Kind::kCharacter);
    if (!leader || !support) return std::nullopt;
    return std::array<PairPlacementMath::Mark, 2>{*leader, *support};
}
std::optional<Placement> Read(RE::ActorHandle initiator, RE::ActorHandle target, int action) {
    const auto marks = ReadStartMarks(initiator, target, action);
    auto self = initiator.get(), other = target.get();
    if (!marks || !self || !other || !other->Is3DLoaded()) return std::nullopt;
    // The same plan the start applies (NativePairAlignment): the target (native leader) keeps its
    // position and turns toward the initiator, which moves onto the authored mark.
    const auto pose = [](RE::Actor* actor) {
        const auto p = actor->GetPosition();
        return PairAlignmentMath::Pose{p.x, p.y, p.z, actor->GetAngleZ()};
    };
    // A player target keeps its heading at the start (NativePairAlignment KeepsHeading): checked the same way.
    const auto plan = PairAlignmentMath::Pivoted(
        {pose(self.get()), pose(other.get())}, (*marks)[0], (*marks)[1], 1, other->IsPlayerRef());
    if (!plan) return std::nullopt;
    const auto& a = plan->goals[0];
    const auto& b = plan->goals[1];
    return Placement{{RE::NiPoint3{a.x, a.y, a.z}, RE::NiPoint3{b.x, b.y, b.z}}, b.yaw, a.yaw};
}
}
