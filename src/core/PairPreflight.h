#pragma once
#include "PCH.h"
#include "core/PairPlacementMath.h"
#include <optional>

namespace AnimationInjector::PairPreflight {
// Ordered native leader/support. Values only; no engine pointer escapes.
std::optional<std::array<PairPlacementMath::Mark, 2>> ReadStartMarks(
    RE::ActorHandle initiator, RE::ActorHandle target, int action);
struct Placement {
    // Indexed by initiator/target, not native lead/support order.
    std::array<RE::NiPoint3, 2> positions;
    float targetYaw = 0;     // The target's heading after the start turns it (0.12.256).
    float initiatorYaw = 0;  // The initiator's heading on its authored mark (0.12.280).
};
// Read resident base bindings under each actor's graph lock. No Queue/Load,
// animation event, transform write, or retained engine pointer.
// Missing/ambiguous bindings and unsupported roles return unavailable.
std::optional<Placement> Read(RE::ActorHandle initiator, RE::ActorHandle target, int action);
}
