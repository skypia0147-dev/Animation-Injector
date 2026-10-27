#pragma once
#include <cstdint>
namespace RE {
class Actor;
}

namespace AnimationInjector::PairStartCamera {
// Installed after other camera hooks. Only a confirmed startup lease may guard
// the camera; ordinary movement must pass through without actor/graph queries.
void Install();
std::uint64_t Acquire(RE::Actor* self, RE::Actor* target);
void Release(std::uint64_t token);
// The lease outlives its start: kept for the pair that plays, until Release; a new start takes it over.
void Hold(std::uint64_t token);

// Keep the live orbit; no body-yaw write or camera reset is needed.
template<class State, class Original>
void SetMode(State& state, bool protectedPlayer, Original original) {
    if (protectedPlayer && !state.toggleAnimCam)
        state.freeRotationEnabled = true;
    else
        original();
}
}
