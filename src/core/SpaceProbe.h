#pragma once
#include "PCH.h"
#include "runtime/ConditionFlow.h"
#include "core/GroundPolicy.h"
namespace AnimationInjector::SpaceProbe {
ConditionFlow::Value EvaluateGround(RE::ActorHandle initiator, RE::ActorHandle target, int candidate, int role,
    float distance, float direction, GroundPolicy::Limits limits);
// Production condition. Uses predicted role positions without moving either actor.
// Direction is relative to the partner's heading after the start turns it (0.12.256: the start keeps
// the partner's position and turns it toward the initiator, so the prediction is the applied plan).
ConditionFlow::Value Evaluate(
    RE::ActorHandle initiator, RE::ActorHandle target, int candidate, int role, float distance, float direction);
// Sweep the actor's own collision shape from where it stands, straight to a destination on the same
// height. The partner never blocks. kFalse = blocked; kUnavailable when the query cannot run.
ConditionFlow::Value Path(std::uint64_t id, RE::Actor* actor, RE::Actor* partner, const RE::NiPoint3& destination);
// Called synchronously before native placement; no live transforms are changed.
void BeforePlacement(std::uint64_t request, RE::ActorHandle initiator, RE::ActorHandle target,
    RE::bhkCharacterController* moving, const RE::hkVector4& destination);
}
