#pragma once
#include "PCH.h"

// CommonLib forward-declares this type. Layout documented by OAR's local
// src/Havok/Havok.h and corroborated by native Start's ActorSyncInfo traversal.
// Read-only: no allocation, destruction, vtable calls or field writes here.
namespace AnimationInjector {
struct SynchronizedSceneView : RE::BSSynchronizedClipGenerator::hkbSynchronizedAnimationScene {
    struct ActorInfo {
        RE::ActorHandle actor;
        std::uint32_t padding;
        RE::BSSynchronizedClipGenerator* generator;
        RE::hkbCharacter* character;
    };
    float unused;
    std::uint32_t numActors;
    RE::BSTSmallArray<ActorInfo, 3> actors;
    RE::BSTSmallArray<RE::ObjectRefHandle, 2> references;
    std::uint32_t numActivated, currentGenerator, lastTimestamp, padding;
};
static_assert(sizeof(SynchronizedSceneView::ActorInfo) == 0x18);
static_assert(sizeof(SynchronizedSceneView) == 0xA0);
static_assert(offsetof(SynchronizedSceneView, actors) == 0x20);
}
