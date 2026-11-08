#pragma once
#include <cstdint>
#include "PCH.h"
#include "core/PairPreflight.h"

namespace AnimationInjector::PairedStartHooks {

// Read-only diagnostics for native calls made by one synchronous paired request.
// Request-scoped actor handles are restored on return; engine pointers stay borrowed.
// Reports base/sync binding presence and exact annotation track matches.
// Also queries SpaceProbe before native TryMoveTo without changing its result.
void Install();
class Attempt {
public:
    // chained: both actors left a pair together just before (a pair-to-pair transition); the alignment then
    // does not probe the attacker's path (a condition card's space check is the author's own).
    explicit Attempt(std::uint64_t request, RE::ActorHandle initiator = {}, RE::ActorHandle target = {},
        int action = -1, bool chained = false);
    ~Attempt();
    Attempt(const Attempt&) = delete;
    Attempt& operator=(const Attempt&) = delete;

private:
    std::uint64_t previous;
    bool previousChained = false;
    RE::ActorHandle previousInitiator, previousTarget;
    std::optional<PairPreflight::Placement> previousPlacement;
};

}
