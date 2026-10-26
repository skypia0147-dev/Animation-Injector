#pragma once
#include "PCH.h"
#include "runtime/ConditionFlow.h"
#include "core/GroundPolicy.h"
namespace AnimationInjector::SpaceProbe {
// Production conditions (0.12.299): from where the initiator stands now, along its current heading turned by
// `direction`. The target may be none; the room check then leaves only the initiator out. Neither actor is moved.
ConditionFlow::Value EvaluateGround(
    RE::ActorHandle initiator, RE::ActorHandle target, float distance, float direction, GroundPolicy::Limits limits);
ConditionFlow::Value Evaluate(RE::ActorHandle initiator, RE::ActorHandle target, float distance, float direction);
// Sweep the actor's own collision shape from where it stands, straight to a destination on the same
// height. The partner never blocks. kFalse = blocked; kUnavailable when the query cannot run.
ConditionFlow::Value Path(std::uint64_t id, RE::Actor* actor, RE::Actor* partner, const RE::NiPoint3& destination);
// Called synchronously before native placement; no live transforms are changed.
void BeforePlacement(std::uint64_t request, RE::ActorHandle initiator, RE::ActorHandle target,
    RE::bhkCharacterController* moving, const RE::hkVector4& destination);
}
