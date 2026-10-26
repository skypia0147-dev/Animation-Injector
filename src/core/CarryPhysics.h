#pragma once
#include <cstdint>

namespace RE {
class bhkCharacterController;
class NiPoint3;
}
namespace AnimationInjector::CarryPhysics {
// Only native character controller families with the inspected setter ABI.
bool Available(RE::bhkCharacterController* controller);
bool WriteFilter(RE::bhkCharacterController* controller, std::uint32_t filter);
// Relocate the non-colliding controller without touching the authored scene pose.
bool Follow(RE::bhkCharacterController* controller, const RE::NiPoint3& position);
}
