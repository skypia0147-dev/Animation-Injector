#pragma once
#include "RE/N/NiSmartPointer.h"
#include <cstdint>

namespace RE {
class Actor;
class NiAVObject;
class NiNode;
class NiTimeController;
class NiTransform;
}

namespace AnimationInjector::CarryAttachment {
// The engine owns allocation and destruction of the mounted-reference controller.
// A lease keeps the exact scene/controller alive so cleanup never removes another
// mod's replacement. Actor references are resolved by the caller for each use.
struct Lease {
    RE::NiPointer<RE::NiAVObject> scene, carrierScene;
    RE::NiPointer<RE::NiNode> anchor;
    RE::NiPointer<RE::NiTimeController> sync;
    std::uint64_t ticks = 0;
};

// Attach only to unclaimed loaded scenes; do not create a mount interaction.
bool Begin(Lease& lease, RE::Actor* carrier, RE::Actor* carried, RE::NiNode* anchor, const RE::NiTransform& offset);
bool Matches(const Lease& lease, RE::Actor* carrier, RE::Actor* carried, RE::NiNode* anchor);
// Explicit scene-only update while the carried actor's AI processing is paused.
bool Pulse(Lease& lease, RE::Actor* carrier, RE::Actor* carried, RE::NiNode* anchor);
// Idempotent, including old scenes retained through unload/3D replacement.
void End(Lease& lease);
}
