#pragma once
#include "PCH.h"

namespace AnimationInjector::AirControl {
// Register only successfully injected air states, using their graph-local events.
void RegisterRoute(RE::hkbBehaviorGraph* a_graph, RE::hkbStateMachine* a_machine, int a_action,
    const std::vector<std::int32_t>& a_states);
void Install();
// Allow the explicit exit-to-fall event through our state-local filter.
bool ResumeFalling(RE::Actor* a_actor);
void ResetOnLoad();
}
