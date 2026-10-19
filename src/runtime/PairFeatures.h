#pragma once
#include "PCH.h"
#include "runtime/PairedSession.h"

namespace AnimationInjector::PairFeatures {
// Mutations run on the player update thread. Hook inputs are copied, never retained.
void Begin(PairedSession::Ticket ticket, int action, RE::ActorHandle initiator, RE::ActorHandle target);
void Suspend(PairedSession::Ticket ticket);
// Accepted movement exits release only the player while the paired source pose blends out.
bool BeginMovementRecovery(PairedSession::Ticket ticket, RE::ActorHandle actor);
void MaintainMovementRecovery();
bool MovementRecoveryActive(PairedSession::Ticket ticket, RE::ActorHandle actor);
// Called only by the native PlayerControls sync query, before event dispatch.
bool AllowRecoveryInput(RE::ActorHandle actor);
// The same accepted-exit ownership also permits the native movement calculation.
bool AllowRecoveryMotion(RE::ActorHandle actor);
// Effects the pair's clip time or its events started end with their run scopes (EventEffects), so a
// role handed to a linked action keeps its hidden weapon until that action shows it or the chain ends.
void End(PairedSession::Ticket ticket, bool completed, bool handoff = false);
void AbortHandoffCamera(PairedSession::Ticket ticket);
// An ordinary action's camera restore (0.12.260): the player's action takes the camera as a pair does, and gives it
// back at its end (restore="end") or on its reset event after it (restore="event", within the timeout).
void BeginActionCamera(RE::ActorHandle player, int action);
void EndActionCamera(RE::ActorHandle player, int action, bool completed);
void Update(float delta);
void ResetOnLoad();
// Main thread: a pair camera shot or its recovery is still ours (runtime/ViewGuard.h waits for it).
bool CameraBusy();
void ObserveEvent(RE::ActorHandle actor, const RE::BSFixedString& event);
void ObserveSync(PairedSession::Ticket ticket, RE::ActorHandle actor, const RE::BSSynchronizedClipGenerator& generator,
    bool exiting);
void ObserveHit(RE::ActorHandle victim, RE::ActorHandle attacker);
// An event function's releasePhysics: this role of the running pair goes back to physics (its flags
// from before the pair are kept). False when the actor is not in the pair.
bool ReleasePhysics(RE::ActorHandle actor);
// An event function's cancel: ends the running pair on the next update when the actor is in it.
void RequestCancel(RE::ActorHandle actor);
bool Translation(RE::ActorHandle actor, int action, float time, RE::NiPoint3& output);
bool Rotation(RE::ActorHandle actor, int action, float time, RE::NiQuaternion& output);
}
