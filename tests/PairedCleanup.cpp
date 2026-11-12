#include "../src/runtime/PairedCleanup.h"

using AnimationInjector::PairedCleanup::IsReleased;
using AnimationInjector::PairedCleanup::GraphState;
using AnimationInjector::PairedCleanup::PlayedOut;
using AnimationInjector::PairedCleanup::StopsPlaying;

constexpr GraphState idle{false, false, false, true};
constexpr GraphState priorAction{false, false, true, true};
// Observed NPC regression: its previous action was driven, but control is free now.
static_assert(IsReleased(priorAction, idle));
static_assert(IsReleased(idle, idle));
static_assert(IsReleased(priorAction, priorAction));
// A newly stuck movement or pair flag must not become a successful cleanup.
static_assert(!IsReleased(idle, priorAction));
static_assert(!IsReleased(priorAction, {true, false, false, true}));
static_assert(!IsReleased(priorAction, {false, true, false, true}));
// Missing graph data cannot prove release, including after participant loss.
static_assert(!IsReleased(priorAction, {}));
static_assert(!IsReleased({}, idle));

using AnimationInjector::PairedCleanup::IsParticipantFinished;
// Observed disable case: target exited with absent 3D; survivor is released.
static_assert(IsParticipantFinished(true, true, false, true, true));
static_assert(IsParticipantFinished(true, true, true, false, true));
// Unloaded is not enough without an exit or the participant-loss reason.
static_assert(!IsParticipantFinished(true, false, false, true, true));
static_assert(!IsParticipantFinished(false, false, false, true, true));
static_assert(!IsParticipantFinished(true, true, false, true, false));
// A loaded survivor with stuck flags or unreadable graph still blocks cleanup.
static_assert(!IsParticipantFinished(true, true, false, false, true));
static_assert(!IsParticipantFinished(true, false, true, false, true));
static_assert(IsParticipantFinished(true, true, true, false, false));
static_assert(IsParticipantFinished(false, false, true, false, false));

// A lost participant stops a playing pair, but not a target its own clip killed (2_KillActor): the initiator
// plays on.
constexpr AnimationInjector::PairedCleanup::Lost here{}, dead{false, true, false}, corpse{false, true, true},
    ragdolled{false, false, true}, gone{true, false, false};
static_assert(!StopsPlaying(here, here, false) && !StopsPlaying(here, here, true));
static_assert(StopsPlaying(here, dead, false) && StopsPlaying(here, ragdolled, false));
static_assert(!StopsPlaying(here, dead, true) && !StopsPlaying(here, corpse, true));
// Its 3D gone, or the initiator lost, stops it whatever killed the target.
static_assert(StopsPlaying(here, gone, true) && StopsPlaying(dead, dead, true) &&
              StopsPlaying(ragdolled, corpse, true) && StopsPlaying(gone, here, false));
// Played out: both clips, or the initiator's alone after its clip killed the target.
static_assert(PlayedOut(true, true, false) && PlayedOut(true, false, true));
static_assert(!PlayedOut(true, false, false) && !PlayedOut(false, true, true) && !PlayedOut(false, false, true));

// A new ordinary runner may own driven=true while its old paired clip exits.
static_assert(IsReleased(idle, priorAction, true));
static_assert(!IsReleased(idle, {true, false, true, true}, true));
static_assert(!IsReleased(idle, {false, true, true, true}, true));
static_assert(!IsReleased(idle, {}, true));
constexpr bool HandoffPermissions() {
    AnimationInjector::PairedCleanup::Handoff h;
    if (h.CanEnter(0, 1)) return false;
    h.destination = {1, 2};
    h.open = true;
    if (!h.CanEnter(0, 1) || !h.CanEnter(1, 2) || h.CanEnter(0, 2) || h.CanEnter(1, 1) || h.CanEnter(-1, 1) ||
        h.CanEnter(2, 1) || h.CanEnter(0, -1))
        return false;
    h.Close(1, false, true);
    if (h.CanEnter(0, 1) || !h.started[0] || h.started[1]) return false;
    h = {};
    return !h.open && !h.started[0] && !h.started[1] && !h.CanEnter(0, 1);
}
static_assert(HandoffPermissions());
