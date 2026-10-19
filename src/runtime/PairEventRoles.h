#pragma once
#include "runtime/EventRegistrySpec.h"

// Who an event function reacts to and whom it changes, as pair role masks (bit 0 initiator, bit 1
// receiver). Pure, for the executor (runtime/EventEffects.h) and its tests.
namespace AnimationInjector::PairEventRoles {
// Whether a function reacts to an event from this sender: `senderRole` 0 initiator, 1 receiver, -1 not
// in a pair.
constexpr bool Accepts(EventFrom from, int senderRole, bool senderIsPlayer) {
    switch (from) {
    case EventFrom::kAny: return true;
    case EventFrom::kPlayer: return senderIsPlayer;
    case EventFrom::kNpc: return !senderIsPlayer;
    case EventFrom::kInitiator: return senderRole == 0;
    case EventFrom::kReceiver: return senderRole == 1;
    }
    return false;
}
// The pair roles a function reaches for an event from pair role `sender` (0 or 1); `playerRole` is the
// player's role in that pair or -1.
constexpr unsigned Targets(EventTarget target, int sender, int playerRole) {
    const bool valid = sender == 0 || sender == 1;
    switch (target) {
    case EventTarget::kSender: return valid ? 1u << sender : 0u;
    case EventTarget::kPartner: return valid ? 1u << (1 - sender) : 0u;
    case EventTarget::kBoth: return valid ? 3u : 0u;
    case EventTarget::kInitiator: return 1u;
    case EventTarget::kReceiver: return 2u;
    case EventTarget::kPlayer: return playerRole == 0 || playerRole == 1 ? 1u << playerRole : 0u;
    }
    return 0;
}
// A clip-time effect has no sender: each role its `from` accepts counts as one, and the union keeps a
// role two senders reach from running twice.
constexpr unsigned TimedTargets(EventFrom from, EventTarget target, int playerRole) {
    unsigned mask = 0;
    for (int sender = 0; sender < 2; ++sender)
        if (Accepts(from, sender, sender == playerRole)) mask |= Targets(target, sender, playerRole);
    return mask;
}
}
