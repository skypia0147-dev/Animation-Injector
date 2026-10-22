#pragma once
#include "runtime/EventRegistrySpec.h"

// Whom an event function changes, as pair role masks (bit 0 initiator, bit 1 receiver). Pure, for the executor
// (runtime/EventEffects.h) and its tests.
namespace AnimationInjector::PairEventRoles {
// The pair roles a function reaches for an event from pair role `sender` (0 or 1); `playerRole` is the
// player's role in that pair or -1.
constexpr unsigned Targets(EventTarget target, int sender, int playerRole) {
    const bool valid = sender == 0 || sender == 1;
    switch (target) {
    case EventTarget::kSender: return valid ? 1u << sender : 0u;
    case EventTarget::kBoth: return valid ? 3u : 0u;
    case EventTarget::kInitiator: return 1u;
    case EventTarget::kReceiver: return 2u;
    case EventTarget::kPlayer: return playerRole == 0 || playerRole == 1 ? 1u << playerRole : 0u;
    }
    return 0;
}
// A clip-time effect has no sender: both roles count as one, and the union keeps a role two senders reach from
// running twice.
constexpr unsigned TimedTargets(EventTarget target, int playerRole) {
    unsigned mask = 0;
    for (int sender = 0; sender < 2; ++sender) mask |= Targets(target, sender, playerRole);
    return mask;
}
}
