#pragma once

#include "PCH.h"
#include <cstdint>

namespace AnimationInjector::PairedSession {

// Baseline and injected routes share the same participant lifecycle.
// One pair is supported at a time. Participants are retained as handles.
enum class Phase : std::uint8_t { kIdle, kPendingStart, kAwaitingBoth, kPlaying, kStopping };

enum class EndReason : std::uint8_t {
    kNone,
    kCompleted,
    kRejected,
    kStartTimeout,
    kPlaybackTimeout,
    kStopTimeout,
    kParticipantUnavailable,
    kCancelled,
    kGameLoad,
    kMovementRecovery
};

// Correlates callbacks and invalidates work queued before a load or new request.
struct Ticket {
    std::uint64_t generation = 0;
    std::uint64_t request = 0;
    bool operator==(const Ticket&) const = default;
};

struct Request {
    int actionIndex = -1;
    RE::ActorHandle initiator;
    RE::ActorHandle target;
    // The caller resolves the form by plugin name and local ID before submission.
    RE::FormID idleForm = 0;
    float startTimeout = 2.0f;
    // These are watchdog limits, not animation lengths or normal exit times.
    float playbackTimeout = 15.0f;
    float stopTimeout = 2.0f;
};

struct ParticipantStatus {
    RE::ActorHandle actor;
    bool entered = false;
    bool exited = false;
    bool endEventSeen = false;
    // Includes a pre-existing driven action releasing control, or an explicit
    // handoff transferring driven ownership to an ordinary runner. Sync/pitch
    // restoration and old clip exits are still required in the handoff case.
    bool graphStateRestored = false;
    // No actor or loaded 3D; distinct from a failed graph-variable read.
    bool graphUnavailable = false;
    float clipDuration = 0.0f;
    // Injected pairs only: on the ground when this pair began (inherited air counts as ground).
    bool startGrounded = false;
    // The controller was in air at start only because the previous pair had just ended.
    // It stays frozen that way while this pair holds the actor; it is not a jump or fall.
    bool inheritedAir = false;
};

struct Status {
    bool exitMovementReturned = false;
    float rootExitBudget = 2.0f;
    bool rootHandoffSuppressed = false;
    Ticket ticket;
    int action = -1;  // the requested action of this ticket
    Phase phase = Phase::kIdle;
    EndReason lastEnd = EndReason::kNone;
    ParticipantStatus initiator;
    ParticipantStatus target;
    float phaseElapsed = 0.0f;
    bool idleCallAccepted = false;
};

// Main-thread entry points. A successful submission is not proof of playback.
// Submit reserves both actors before any synchronous engine callback can occur.
// Vanilla mode does not require or create injected graph nodes or idle records.
[[nodiscard]] bool Submit(const Request& a_request, Ticket& a_ticket);
void Update(float a_gameDelta);
void Cancel(Ticket a_ticket);
// Release input during an already-running normal exit; never restart its transition.
bool BeginNormalExitMovement(Ticket ticket, RE::ActorHandle player, int action);

// Reserve exact ordinary destinations for one synchronous stop/start handoff.
// Old sync clips remain observed until their exits; released features must never
// restore state over the new runners. No other action may bypass the pair guard.
[[nodiscard]] bool BeginOrdinaryHandoff(Ticket ticket, RE::ActorHandle actor, int action, int partnerAction);
[[nodiscard]] bool CanEnterHandoff(RE::ActorHandle actor, int action);
void CompleteOrdinaryHandoff(Ticket ticket, RE::ActorHandle actor, bool actorStarted, bool partnerStarted);

// Exact ordinary destination protected while this pair is retiring; no runner pointers escape.
int RetiringHandoffDestination(const RE::hkbContext& context, RE::ActorHandle& actor);

// Invalidates tickets and drops observations before the next save can run.
// Must not restore old-save state into actors resolved from the new save.
void ResetOnLoad();

// Thread-safe snapshots. Hooks must never retain a pointer to ActionRunner.
[[nodiscard]] Status ReadStatus();
[[nodiscard]] bool TryGetTicket(RE::ActorHandle a_actor, Ticket& a_ticket);
[[nodiscard]] bool TryGetParticipant(const RE::hkbContext& a_context, RE::ActorHandle& a_actor, Ticket& a_ticket);

// Called from existing hooks with the real engine context. Arguments are borrowed
// only for this call. Implementations copy bounded observations into a synchronized
// queue; actor mutations, logging and state transitions are drained by Update.
// Match the expected actor, role and generator before accepting an observation.
// A sync Activate callback alone is insufficient: verify a loaded internal clip.
void ObserveSyncActivate(Ticket a_ticket, RE::ActorHandle a_actor, const RE::hkbContext& a_context,
    const RE::BSSynchronizedClipGenerator& a_generator);
void ObserveSyncDeactivate(Ticket a_ticket, RE::ActorHandle a_actor, const RE::hkbContext& a_context,
    const RE::BSSynchronizedClipGenerator& a_generator);
void ObserveEvent(Ticket a_ticket, RE::ActorHandle a_actor, const RE::BSFixedString& a_event);

}  // namespace AnimationInjector::PairedSession
