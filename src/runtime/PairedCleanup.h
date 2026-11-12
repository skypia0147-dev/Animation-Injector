#pragma once
#include <array>

namespace AnimationInjector::PairedCleanup {
struct GraphState {
    bool synced = false, pitch = false, driven = false, valid = false;
};

// Read-only release policy, applied together with both clips' exit evidence.
// An NPC's preceding animation-driven action may finish during the pair.
// Its true -> false change is released control, not a reason to restore true.
// Still reject a newly stuck driven flag, sync flag, or pitch lock.
constexpr bool IsReleased(const GraphState& before, const GraphState& after, bool drivenTransferred = false) {
    return before.valid && after.valid && before.synced == after.synced && before.pitch == after.pitch &&
           (drivenTransferred || before.driven || !after.driven);
}

// Request permission exists only during the synchronous handoff. After closing,
// only successfully started runners own the driven flag; sync/pitch still matter.
struct Handoff {
    std::array<int, 2> destination{-1, -1};
    std::array<bool, 2> started{};
    bool open = false;
    constexpr bool CanEnter(int role, int action) const {
        return open && role >= 0 && role < 2 && action >= 0 && destination[role] == action;
    }
    constexpr int ProtectedDestination(int role) const {
        return role >= 0 && role < 2 && (open || started[role]) ? destination[role] : -1;
    }
    constexpr void Close(int role, bool actorStarted, bool partnerStarted) {
        if (!open || role < 0 || role > 1) return;
        started[role] = actorStarted;
        started[1 - role] = partnerStarted;
        open = false;
    }
};

// A participant as the playing pair sees it: gone (no actor or no loaded 3D), dead, or in ragdoll.
struct Lost {
    bool gone = false, dead = false, ragdoll = false;
    constexpr bool Any() const { return gone || dead || ragdoll; }
};
// Whether a playing pair stops because a participant was lost. A target its own clip killed (the 2_KillActor
// annotation, as a vanilla kill move's victim) is no loss while the initiator can play: the target leaves the
// animation (it dies and ragdolls) and the initiator plays on to its clip's end (1.0.8). A target lost any other way
// (a hit from outside, a ragdoll, its 3D gone) stops the pair, as does a lost initiator.
constexpr bool StopsPlaying(const Lost& initiator, const Lost& target, bool targetKilledByClip) {
    if (initiator.Any() || target.gone) return true;
    return target.Any() && !targetKilledByClip;
}
// Whether a playing pair has played to its end: both clips exited, or the initiator's alone once its clip killed
// the target (a corpse's clip may never report its exit).
constexpr bool PlayedOut(bool initiatorExited, bool targetExited, bool targetKilledByClip) {
    return initiatorExited && (targetExited || targetKilledByClip);
}

// Missing graphs are not restored graphs. Only a participant-loss stop with an
// observed clip exit may finish without reading that participant's absent 3D.
constexpr bool IsParticipantFinished(
    bool entered, bool exited, bool restored, bool graphUnavailable, bool participantLoss) {
    return (!entered || exited) && (restored || (participantLoss && graphUnavailable && exited));
}
}
