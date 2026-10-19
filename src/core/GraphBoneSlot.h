#pragma once
#include "PCH.h"
#include <cstdint>

namespace AnimationInjector::GraphBoneSlot {
// A bone the animation graph drives lives either in a plain skeleton node or in an entry of a
// flattened bone tree (BShkbAnimationGraph::boneNodes: node + entry index, -1 = plain node).
struct Slot {
    RE::NiTransform *local = nullptr, *mirror = nullptr;
    const RE::NiTransform *world = nullptr, *parent = nullptr;
};
inline Slot Access(RE::NiAVObject* owner, std::int32_t entry) {
    if (!owner) return {};
    if (entry < 0) return {&owner->local, nullptr, &owner->world, owner->parent ? &owner->parent->world : nullptr};
    auto* tree = netimmerse_cast<RE::BSFlattenedBoneTree*>(owner);
    if (!tree) return {};
    auto& data = tree->GetRuntimeData();
    if (!data.boneEntries || static_cast<std::uint32_t>(entry) >= data.numBones) return {};
    auto& e = data.boneEntries[entry];
    const auto* parent = e.parentIndex >= 0 && static_cast<std::uint32_t>(e.parentIndex) < data.numBones
                             ? &data.boneEntries[e.parentIndex].world
                             : &tree->world;
    // The engine's pose copy writes the mapped node when one exists; keep the entry equal.
    if (e.node) return {&e.node->local, &e.local, &e.node->world, parent};
    return {&e.local, nullptr, &e.world, parent};
}
// Name of a graph bone entry (plain node name, or the flattened entry's name).
inline RE::BSFixedString Name(RE::NiAVObject* owner, std::int32_t entry) {
    if (!owner) return {};
    if (entry < 0) return owner->name;
    auto* tree = netimmerse_cast<RE::BSFlattenedBoneTree*>(owner);
    if (!tree) return {};
    const auto& data = tree->GetRuntimeData();
    if (!data.boneEntries || static_cast<std::uint32_t>(entry) >= data.numBones) return {};
    return data.boneEntries[entry].nodeName;
}
}
