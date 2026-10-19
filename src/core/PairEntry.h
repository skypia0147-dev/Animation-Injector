#pragma once
#include <cstddef>
#include <cstdint>

#include "RE/B/BSFixedString.h"
#include "core/PairEntryNames.h"

namespace RE {
class Actor;
}

// Shared pair entry (step 2 after core/FullBodyEntry.h, docs/최적화-공용진입-0.12.209.md).
//
// Every paired action adds two states to RootBehaviorGraph (lead and support role) and a root
// wildcard for each (event = that role's enter event). With 5000 test declarations those are 3,248
// of the ~3,366 root wildcards left after 0.12.209, and the engine checks every root wildcard for
// every event the root handles, on every character, all the time.
//
// The paired states move into two containers, one per role, which GraphInjector adds to the root
// once per character graph:
//   AI_PairLead     root state -> nested machine AI_PairLead_Behavior
//   AI_PairSupport  root state -> nested machine AI_PairSupport_Behavior
// The root gets two wildcards in total (AI_PairEnterLead, AI_PairEnterSupport): copies of the
// native role entries (VampireFeedStanding_Front_Victim / _Player) aimed at the container without a
// nested target. The nested machine's start state is bound to iAI_PairState (a copy of a native
// startStateId binding, as for FullBodyPlayerIdles_Behavior).
//
// Inside a container, unchanged: the paired states (activity wrapper around the synchronized clip,
// named after the enter event; the engine finds that clip by name when SetupSpecialIdle adds the
// actors, as it finds vanilla kill moves nested in 1hm_behavior), and one inner wildcard per state
// for its enter event, used while the container is still active (re-entry during an exit blend).
//
// Exits move from each paired state to the container state, one transition per distinct event:
//   the native role exit (PairEnd / pairedStop) and every action's exit event  -> fAI_PairExitBlend
//   every action's partner-loss exit                                          -> fAI_PairLossBlend
//   every action's recovery event                                             -> fAI_PairRecoveryBlend
// Each kind uses one effect per role: a copy of the native role exit whose duration is bound to that
// variable (a copy of a native duration binding). Priorities stay as before (loss and recovery at
// least the wildcard priority). An action that authors no blend gets the native exit duration, so its
// exit is the native one, as before. The role's native notify events (lead: PairedKillTarget and
// HeadTrackingOff on enter, HeadTrackingOn on exit; support: 2_PairEnd on exit) move to the
// container state, which the root enters and leaves exactly when it entered and left a paired state.
//
// Senders do not change. The engine's paired start sends each participant its role's enter event
// through NotifyAnimationGraph ("X" to the lead, "pa_X" to the support); a registered event makes
// BeforeNotify set the four variables on that actor and send the role's shared entry, then the hook
// forwards the original event as before (the inner wildcard; ignored when that state is current).
// The containers are the only route (0.12.212): a graph that lacks a template they need gets no pair,
// and the reason is logged once ("[PairEntry] pair containers unavailable").
namespace AnimationInjector::PairEntry {
enum class Role : std::uint8_t { kLead, kSupport };

// Blend seconds of one action, set on the actor at its entry. An unauthored blend is the native
// role exit's duration.
struct Blends {
    float exit = 0.0f;
    float loss = 0.0f;
    float recovery = 0.0f;
};

// Graph side (injection pass, loader thread): `enterEvent` (one role's enter event) enters container
// `role` at nested state `stateId`. Stands for that role's own root wildcard.
void Register(const char* enterEvent, Role role, std::int32_t stateId, const Blends& blends);
// Number of registered entries (log and tests).
std::size_t Count();

// Runtime (NotifyAnimationGraph hook, any thread).
struct Sent {
    bool sent = false;      // `event` is a registered entry and the shared entry was sent
    bool accepted = false;  // the graph accepted the shared entry
};
// Before `event` is forwarded: when it is a registered entry, sets iAI_PairState and the three blend
// variables on `actor` and sends the role's shared entry. The hook reports the event as accepted
// when either the shared entry or the original event was accepted.
Sent BeforeNotify(RE::Actor* actor, const RE::BSFixedString& event);
}
