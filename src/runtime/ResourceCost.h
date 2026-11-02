#pragma once
#include <algorithm>

namespace RE {
class Actor;
enum class ActorValue;
}

namespace AnimationInjector::ResourceCost {
// The health an action's cost takes (10-31: <Cost health>, op="cost" health): never the last point, so paying for an
// action never kills.
inline float Health(float cost, float current) { return std::clamp(cost, 0.0f, std::max(0.0f, current - 1.0f)); }

// An action's or an event function's cost: stamina, magicka and health damaged by those amounts, or (`percent`) by
// those percent of each maximum (main thread).
void Spend(RE::Actor* actor, float stamina, float magicka, float health, bool percent);
// A resource's maximum: its permanent value with temporary changes (the resource condition's percent reads it too).
float Maximum(RE::Actor* actor, RE::ActorValue value);
// The amount `percent` of a maximum (10-31: measure="percent"), the percent kept within 0~100.
inline float OfMaximum(float percent, float maximum) {
    return maximum > 0.0f ? std::clamp(percent, 0.0f, 100.0f) / 100.0f * maximum : 0.0f;
}
}  // namespace AnimationInjector::ResourceCost
