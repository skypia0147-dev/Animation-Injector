#pragma once
#include <cstddef>
#include <cstdint>

#include "RE/B/BSFixedString.h"
#include "core/FullBodyEntryNames.h"

namespace RE {
class Actor;
}

// Shared full-body entry (0.12.209).
//
// Until 0.12.208 every full-body action added its own wildcard to RootBehaviorGraph for each entry
// clip and each incoming link (event = that enter event, target = FullBodyPlayerIdles with the
// action's state as the nested target). The engine checks every root wildcard for every event the
// root handles, on every character, all the time: with 5000 actions (7,112 such wildcards) a root
// event took ~110 us instead of ~4 us.
//
// From 0.12.209 RootBehaviorGraph gets two wildcards in total (GraphInjector builds them once per
// character graph):
//   AI_FullBodyEnter      entries of actions (the native entry blend's event mode, as before)
//   AI_FullBodyLinkEnter  entries through a link (event mode 1, as link entries used)
// Both go to FullBodyPlayerIdles without a nested target, so its nested machine
// (FullBodyPlayerIdles_Behavior) starts at its start state. That start state is bound to the graph
// variable iAI_FullBodyState, and the shared entries' blend duration to fAI_FullBodyBlend (copies of
// native startStateId / duration bindings of the same graph). Vanilla never enters
// FullBodyPlayerIdles without a nested target (its four root entries all name one), and the
// variable's default 0 is the machine's native start state.
//
// Unchanged: the action states inside FullBodyPlayerIdles_Behavior, their inner wildcards (entering
// or switching while already inside), their exits on the parent, their ids and results. The shared
// entries are the only route (0.12.212): a graph that lacks a template they need gets no full-body
// action, and the reason is logged once.
//
// Senders do not change. Every enter event passes the NotifyAnimationGraph hook; for a registered
// entry BeforeNotify sets both variables on that actor and sends the shared event, then the hook
// forwards the original event as before (inner wildcards and first-person graphs use it).
namespace AnimationInjector::FullBodyEntry {
// Graph side (injection pass, loader thread): `enterEvent` (an entry clip's enter event, or an
// incoming link's) enters nested state `stateId` with `blend` seconds, through the link entry when
// `link`. Stands for that entry's own root wildcard.
void Register(const char* enterEvent, std::int32_t stateId, float blend, bool link);
// Number of registered entries (log and tests).
std::size_t Count();

// Runtime (NotifyAnimationGraph hook, any thread).
struct Sent {
    bool sent = false;      // `event` is a registered entry and the shared event was sent
    bool accepted = false;  // the graph accepted the shared event
};
// Before `event` is forwarded: when it is a registered entry, sets iAI_FullBodyState and
// fAI_FullBodyBlend on `actor` and sends the matching shared event. The hook reports the event as
// accepted when either the shared event or the original one was accepted.
Sent BeforeNotify(RE::Actor* actor, const RE::BSFixedString& event);
}
