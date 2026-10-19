#pragma once
#include "PCH.h"
#include <vector>

namespace AnimationInjector {
struct ActionSpec;
}
namespace AnimationInjector::EventSuppression {
// Immutable event lookup, prepared once from the loaded declarations (the registry passes them
// while it publishes them, before any reader can see them).
void Load(const std::vector<ActionSpec>& specs);
bool Enabled();
bool Consume(RE::Actor* actor, const RE::BSFixedString& event);
// Scope the acceptance result to one native PlayerControls input dispatch.
void BeginInput();
void EndInput();
void RecordRequest(int source, int destination, bool accepted);
}
