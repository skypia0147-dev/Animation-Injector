#include "runtime/PairedSession.h"
#include "runtime/Carry.h"
#include "core/PairedStartHooks.h"
#include <mutex>
#include "runtime/ActionRegistry.h"
#include "runtime/ActionRunner.h"
#include "core/GraphInjector.h"
#include "runtime/PairedCleanup.h"
#include "runtime/PairFeatures.h"
#include "Settings.h"
#include "runtime/InputSink.h"
#include "runtime/PairRecoveryGate.h"
#include "runtime/PairStartAir.h"
#include "runtime/PairStartControl.h"

namespace AnimationInjector::PairedSession {
namespace {
std::mutex mutex;
Status status;
Request request;
std::array<RE::BSFixedString, 2> expectedNames;
RE::BSFixedString customEndEvent;
RE::BSFixedString recoveryEvent, recoveryExitEvent;
PairRecoveryGate movementRecovery;
std::uint64_t generation = 1, sequence = 0;
using GraphState = PairedCleanup::GraphState;
std::array<GraphState, 2> initial;
std::array<std::array<std::uintptr_t, 8>, 2> characters{};
struct Observation {
    int participant = 0;
    bool exit = false, loaded = false, endEvent = false, sample = false, event = false;
    float duration = 0.0f;
    float localTime = -1.0f, controlTime = -1.0f;
    std::uint64_t order = 0;
    std::uintptr_t scene = 0;
    std::uintptr_t generator = 0, character = 0;
    int clipIndex = -1, syncIndex = -1;
    std::array<char, 192> file{};
    std::array<char, 192> bindingFile{};
    std::array<char, 96> eventName{};
};
std::array<Observation, 64> pending;
std::size_t pendingCount = 0;
std::array<Observation, 64> tracePending;
std::size_t tracePendingCount = 0;
bool overflow = false;
std::array<bool, 2> observed{}, loaded{}, deactivated{};
std::uint64_t traceOrder = 0;
std::array<float, 2> lastSampleTime{-1.0f, -1.0f};
std::array<unsigned, 2> sampleCount{};
struct ActorTrace {
    bool available = false, dead = false, ragdoll = false;
    int knock = -1;
    bool operator==(const ActorTrace&) const = default;
};
std::array<ActorTrace, 2> actorTrace{};
std::array<bool, 2> actorTraceSeen{};
RE::BSFixedString lossExitEvent;
float lossBlend = -1.0f;
bool lossBlendRequested = false;
float nextPlacementTrace = 0;
PairedCleanup::Handoff handoff;
bool featuresReleased = false;
// Participants of the last ended pair, for a pair that starts before their controllers land.
struct RecentExit {
    RE::ActorHandle actor;
    PairStartAir::Exit exit;
};
std::array<RecentExit, 2> recentExits{};
// TDM's player yaw while a pair the player plays runs (0.12.279): TDM would turn the player to the crosshair
// (blocking, bashing, aiming) away from the alignment. Held by the ticket that asked; a connected pair takes it over.
PairStartControl::YawLease yawLease;
std::uint64_t yawOwner = 0;

void TracePlacement(std::uint64_t id, const char* phase, float time, RE::Actor* self, RE::Actor* target) {
    if (!FrameworkSettings::Get().debug) return;
    const float inverseScale = RE::bhkWorld::GetWorldScaleInverse();
    for (int role = 0; role < 2; ++role) {
        auto* actor = role == 0 ? self : target;
        if (!actor || !actor->Is3DLoaded()) continue;
        const auto position = actor->GetPosition();
        RE::hkVector4 physical{};  // zero when there is no controller (2026-10-28 review R-27)
        auto* controller = actor->GetCharController();
        if (controller) controller->GetPositionImpl(physical, true);
        const auto& xyz = physical.quad.m128_f32;
        logger::debug(
            "[PairPlacement] #{} phase={} t={:.3f} role={} actor={:08X} handle={:08X} combatTargetHandle={:08X} actorPos=({:.2f},{:.2f},{:.2f}) yaw={:.5f} controller={} controllerPos=({:.2f},{:.2f},{:.2f})",
            id, phase, time, role, actor->GetFormID(), actor->GetHandle().native_handle(),
            actor->GetActorRuntimeData().currentCombatTarget.native_handle(), position.x, position.y, position.z,
            actor->GetAngleZ(), controller != nullptr, xyz[0] * inverseScale, xyz[1] * inverseScale,
            xyz[2] * inverseScale);
    }
}

const RE::BSFixedString& Synced() {
    static const RE::BSFixedString v("bIsSynced");
    return v;
}
const RE::BSFixedString& Pitch() {
    static const RE::BSFixedString v("DisablePitch");
    return v;
}
const RE::BSFixedString& Driven() {
    static const RE::BSFixedString v("bAnimationDriven");
    return v;
}
const RE::BSFixedString& Stop() {
    static const RE::BSFixedString v("pairedStop");
    return v;
}
const RE::BSFixedString& End() {
    static const RE::BSFixedString v("PairEnd");
    return v;
}
const RE::BSFixedString& NpcStop() {
    static const RE::BSFixedString v("NPCpairedStop");
    return v;
}
const RE::BSFixedString& NpcEnd() {
    static const RE::BSFixedString v("NPCPairEnd");
    return v;
}
const RE::BSFixedString& TargetEnd() {
    static const RE::BSFixedString v("2_PairEnd");
    return v;
}

GraphState ReadGraph(RE::Actor* a_actor) {
    GraphState s;
    if (!a_actor || !a_actor->Is3DLoaded()) return s;
    const bool a = a_actor->GetGraphVariableBool(Synced(), s.synced);
    const bool b = a_actor->GetGraphVariableBool(Pitch(), s.pitch);
    const bool c = a_actor->GetGraphVariableBool(Driven(), s.driven);
    s.valid = a && b && c;
    return s;
}
int Participant(RE::ActorHandle a_actor) {
    if (a_actor == status.initiator.actor) return 0;
    if (a_actor == status.target.actor) return 1;
    return -1;
}
void Push(const Observation& a_observation) {
    if (pendingCount < pending.size()) {
        pending[pendingCount] = a_observation;
        pending[pendingCount++].order = ++traceOrder;
    } else
        overflow = true;
}
// Diagnostic traffic must never exhaust the lifecycle queue or cancel a pair.
void PushTrace(const Observation& a_observation) {
    if (tracePendingCount < tracePending.size()) {
        tracePending[tracePendingCount] = a_observation;
        tracePending[tracePendingCount++].order = ++traceOrder;
    }
}
void CaptureClock(Observation& o, const RE::BSSynchronizedClipGenerator& generator) {
    o.scene = reinterpret_cast<std::uintptr_t>(generator.synchronizedScene);
    if (auto* clip = generator.clipGenerator) {
        o.localTime = clip->localTime;
        if (auto* control = clip->animationControl.get()) {
            o.controlTime = control->localTime;
            if (control->binding && control->binding->animation) o.duration = control->binding->animation->duration;
        }
    }
}
// Actor state is sampled on the main thread; it is not the exact death instant.
void TraceActor(int role, RE::Actor* actor) {
    ActorTrace current;
    current.available = actor && actor->Is3DLoaded();
    if (current.available) {
        current.dead = actor->IsDead();
        current.ragdoll = actor->IsInRagdollState();
        if (auto* state = actor->AsActorState()) current.knock = static_cast<int>(state->GetKnockState());
    }
    std::lock_guard lock(mutex);
    if (actorTraceSeen[role] && actorTrace[role] == current) return;
    actorTraceSeen[role] = true;
    actorTrace[role] = current;
    logger::debug(
        "[PairTrace] #{} order={} actor-state role={} available={} dead={} ragdoll={} knock={} phase={} phaseTime={:.3f}",
        status.ticket.request, ++traceOrder, role, current.available, current.dead, current.ragdoll, current.knock,
        static_cast<int>(status.phase), status.phaseElapsed);
}
void Finish(EndReason a_reason) {
    if (!featuresReleased)
        PairFeatures::End(status.ticket, a_reason == EndReason::kCompleted || a_reason == EndReason::kCancelled ||
                                             a_reason == EndReason::kMovementRecovery);
    std::lock_guard lock(mutex);
    if (featuresReleased)
        logger::debug(
            "[PairHandoff] #{} cleanup reason={} elapsed={:.3f} drivenTransferred={}/{} exits={}/{} released={}/{}; no feature restore over destination",
            status.ticket.request, static_cast<int>(a_reason), status.phaseElapsed, handoff.started[0],
            handoff.started[1], status.initiator.exited, status.target.exited, status.initiator.graphStateRestored,
            status.target.graphStateRestored);
    if (lossBlendRequested)
        logger::debug("[PairLossBlend] #{} cleanup ended reason={} stopElapsed={:.3f} requestedBlend={:.3f}",
            status.ticket.request, static_cast<int>(a_reason), status.phaseElapsed, lossBlend);
    logger::info("[PairB] #{} 종료 reason={} 양쪽진입={}/{} 양쪽종료={}/{} 그래프복귀={}/{} graphUnavailable={}/{}",
        status.ticket.request, static_cast<int>(a_reason), status.initiator.entered, status.target.entered,
        status.initiator.exited, status.target.exited, status.initiator.graphStateRestored,
        status.target.graphStateRestored, status.initiator.graphUnavailable, status.target.graphUnavailable);
    status.lastEnd = a_reason;
    if (yawOwner == status.ticket.request) {
        yawLease.Release();
        yawOwner = 0;
    }
    recentExits = {RecentExit{status.initiator.actor, {status.initiator.startGrounded, 0.0f}},
        RecentExit{status.target.actor, {status.target.startGrounded, 0.0f}}};
    status.phase = Phase::kIdle;
    handoff = {};
    featuresReleased = false;
    pendingCount = 0;
    tracePendingCount = 0;
    characters = {};
}
void BeginStop(EndReason a_reason) {
    Status copy;
    std::uint64_t order = 0;
    std::array<bool, 2> activeNodes;
    {
        std::lock_guard lock(mutex);
        if (status.phase == Phase::kIdle || status.phase == Phase::kStopping) return;
        status.phase = Phase::kStopping;
        status.phaseElapsed = 0.0f;
        status.lastEnd = a_reason;
        copy = status;
        order = ++traceOrder;
        activeNodes = {observed[0] && !deactivated[0], observed[1] && !deactivated[1]};
    }
    PairFeatures::Suspend(copy.ticket);
    logger::debug("[PairB] #{} 정리 시작: 최초사유={} 활성노드={}/{}", copy.ticket.request, static_cast<int>(a_reason),
        activeNodes[0], activeNodes[1]);
    logger::debug(
        "[PairTrace] #{} order={} AI-BeginStop reason={}", copy.ticket.request, order, static_cast<int>(a_reason));
    auto self = copy.initiator.actor.get(), target = copy.target.actor.get();
    const std::array<RE::Actor*, 2> actors{self.get(), target.get()};
    std::array<bool, 2> ready{}, lost{}, gentle{};
    for (int i = 0; i < 2; ++i) {
        ready[i] = actors[i] && actors[i]->Is3DLoaded();
        lost[i] = ready[i] && (actors[i]->IsDead() || actors[i]->IsInRagdollState());
    }
    if (a_reason == EndReason::kParticipantUnavailable && lossBlend > 0.0f && copy.initiator.entered &&
        copy.target.entered) {
        for (int i = 0; i < 2; ++i) gentle[i] = ready[i] && !lost[i] && lost[1 - i] && activeNodes[i];
        if (gentle[0] || gentle[1]) {
            lossBlendRequested = true;
            // The watchdog must permit the requested blend to finish.
            request.stopTimeout = std::max(request.stopTimeout, lossBlend + 1.0f);
        }
    }
    RE::ActorHandle recoveredPlayer;
    // Ask only participants of this request to leave the known paired state.
    // Never force arbitrary controls or graph booleans back to guessed values.
    for (int i = 0; i < 2; ++i) {
        const auto& part = i == 0 ? copy.initiator : copy.target;
        auto actor = part.actor.get();
        if (!actor || !actor->Is3DLoaded() || !activeNodes[i]) continue;
        // Let death/ragdoll physics proceed on the lost actor. Only its
        // healthy partner receives the dedicated transition to the idle state.
        if (lossBlendRequested && lost[i]) continue;
        const auto& event = gentle[i]                                  ? lossExitEvent
                            : a_reason == EndReason::kMovementRecovery ? recoveryExitEvent
                            : i == 0                                   ? Stop()
                                                                       : End();
        {
            std::lock_guard lock(mutex);
            logger::debug("[PairTrace] #{} order={} AI-notify role={} event='{}'", copy.ticket.request, ++traceOrder, i,
                event.c_str());
        }
        const bool sent = actor->NotifyAnimationGraph(event);
        if (sent && a_reason == EndReason::kMovementRecovery && actor->IsPlayerRef())
            recoveredPlayer = actor->GetHandle();
        if (a_reason == EndReason::kMovementRecovery && !sent) {
            const bool fallback = actor->NotifyAnimationGraph(i == 0 ? Stop() : End());
            logger::warn("[PairMoveRecovery] #{} role={} recovery rejected; standard exit fallback={}",
                copy.ticket.request, i, fallback);
        }
        if (gentle[i]) {
            logger::debug(
                "[PairLossBlend] #{} survivorRole={} actor={:08X} event='{}' notify={} seconds={:.3f} partnerDead={} partnerRagdoll={}",
                copy.ticket.request, i, actor->GetFormID(), event.c_str(), sent, lossBlend, actors[1 - i]->IsDead(),
                actors[1 - i]->IsInRagdollState());
            if (!sent) {
                const bool fallback = actor->NotifyAnimationGraph(i == 0 ? Stop() : End());
                logger::warn(
                    "[PairLossBlend] #{} long exit rejected; standard exit fallback={}", copy.ticket.request, fallback);
            }
        }
        logger::debug("[PairB] #{} 정리요청 actor={:08X} notify={}", copy.ticket.request, actor->GetFormID(), sent);
    }
    if (recoveredPlayer && PairFeatures::BeginMovementRecovery(copy.ticket, recoveredPlayer))
        Carry::ReleaseMovementControl(copy.ticket, recoveredPlayer);
}
}

bool Submit(const Request& a_request, Ticket& a_ticket) {
    a_ticket = {};
    if (a_request.actionIndex < 0 || a_request.actionIndex >= Actions::Count() ||
        Actions::Spec(a_request.actionIndex).clips.size() != 2)
        return false;
    auto self = a_request.initiator.get(), target = a_request.target.get();
    if (!self || !target || self.get() == target.get() || !self->Is3DLoaded() || !target->Is3DLoaded() ||
        self->IsDead() || target->IsDead() || !a_request.idleForm)
        return false;
    std::array<GraphState, 2> before{ReadGraph(self.get()), ReadGraph(target.get())};
    if (FrameworkSettings::Get().debug)
        for (int i = 0; i < 2; ++i) {
            auto* actor = i == 0 ? self.get() : target.get();
            auto* base = actor->GetActorBase();
            auto* state = actor->AsActorState();
            auto* package = actor->GetCurrentPackage();
            const auto& b = before[i];
            logger::debug(
                "[PairB] 시작조건 role={} actor={:08X} name='{}' sex={} attack={} sit={} knock={} package={:08X} graphValid={} synced/pitch/driven={}/{}/{}",
                i, actor->GetFormID(), actor->GetDisplayFullName(), base ? static_cast<int>(base->GetSex()) : -1,
                state ? static_cast<int>(state->GetAttackState()) : -1,
                state ? static_cast<int>(state->GetSitSleepState()) : -1,
                state ? static_cast<int>(state->GetKnockState()) : -1, package ? package->GetFormID() : 0, b.valid,
                b.synced, b.pitch, b.driven);
        }
    if (!before[0].valid || !before[1].valid || before[0].synced || before[1].synced) {
        logger::warn("[PairB] 거부: 그래프 상태를 읽을 수 없거나 이미 페어 실행 중");
        return false;
    }
    std::array<std::array<std::uintptr_t, 8>, 2> ids{};
    for (int i = 0; i < 2; ++i) {
        auto* actor = i == 0 ? self.get() : target.get();
        RE::BSAnimationGraphManagerPtr manager;
        if (!actor->GetAnimationGraphManager(manager) || !manager) return false;
        RE::BSSpinLockGuard graphLock(manager->GetRuntimeData().updateLock);
        std::size_t j = 0;
        for (const auto& graph : manager->graphs) {
            if (graph && j < ids[i].size()) {
                ids[i][j++] = reinterpret_cast<std::uintptr_t>(&graph->characterInstance);
                auto* setup = graph->characterInstance.setup.get();
                auto* data = setup ? setup->data.get() : nullptr;
                auto* strings = data ? data->stringData.get() : nullptr;
                logger::debug("[PairB] 그래프 role={} slot={} active={} character={:X} project='{}' behavior='{}'", i,
                    j - 1, manager->GetRuntimeData().activeGraph, ids[i][j - 1],
                    strings && strings->name.c_str() ? strings->name.c_str() : "?",
                    strings && strings->behaviorFilename.c_str() ? strings->behaviorFilename.c_str() : "?");
            }
        }
        if (j == 0) return false;
    }
    // Initialize cached event names on the main thread before hooks can use them.
    Stop();
    End();
    NpcStop();
    NpcEnd();
    TargetEnd();
    {
        std::lock_guard lock(mutex);
        if (status.phase != Phase::kIdle) return false;
        request = a_request;
        handoff = {};
        featuresReleased = false;
        lossBlend = Actions::Spec(a_request.actionIndex).paired.lossBlend;
        lossExitEvent = lossBlend > 0.0f
                            ? RE::BSFixedString(Actions::Spec(a_request.actionIndex).paired.lossExitEvent.c_str())
                            : RE::BSFixedString();
        lossBlendRequested = false;
        movementRecovery = {};
        recoveryEvent = RE::BSFixedString();
        recoveryExitEvent = RE::BSFixedString();
        status = {};
        status.action = a_request.actionIndex;
        {
            const auto& spec = Actions::Spec(a_request.actionIndex);
            customEndEvent = RE::BSFixedString(spec.events.exit.c_str());
            movementRecovery.enabled = spec.recovery.enabled && spec.recovery.move;
            movementRecovery.at = spec.recovery.at;
            recoveryEvent = RE::BSFixedString(spec.recovery.event.c_str());
            recoveryExitEvent = RE::BSFixedString(spec.recovery.recoverEvent.c_str());
            status.rootExitBudget = std::max({spec.exitBlend, spec.paired.lossBlend,
                                        spec.recovery.enabled ? spec.recovery.blend : -1.0f, 1.0f}) +
                                    1.0f;
            request.stopTimeout = std::max(request.stopTimeout, status.rootExitBudget);
            expectedNames = {RE::BSFixedString(spec.clips[1].enterEvent.c_str()),
                RE::BSFixedString(spec.clips[0].enterEvent.c_str())};
        }
        status.ticket = {generation, ++sequence};
        status.phase = Phase::kPendingStart;
        status.initiator.actor = a_request.initiator;
        status.target.actor = a_request.target;
        initial = before;
        characters = ids;
        pendingCount = 0;
        tracePendingCount = 0;
        overflow = false;
        observed = {};
        loaded = {};
        deactivated = {};
        traceOrder = 0;
        lastSampleTime = {-1.0f, -1.0f};
        sampleCount = {};
        actorTrace = {};
        actorTraceSeen = {};
        nextPlacementTrace = 0;
        a_ticket = status.ticket;
        if (self->IsPlayerRef() || target->IsPlayerRef()) {
            yawLease.Acquire();  // kept as is when a connected pair still holds it
            yawOwner = a_ticket.request;
        }
    }
    PairFeatures::Begin(a_ticket, a_request.actionIndex, a_request.initiator, a_request.target);
    logger::debug("[PairB] #{} 예약: action='{}' player={:08X} target={:08X} idle={:08X} OAR={}, injected-front",
        a_ticket.request, Actions::Spec(a_request.actionIndex).id, self->GetFormID(), target->GetFormID(),
        a_request.idleForm, REX::W32::GetModuleHandleW(L"OpenAnimationReplacer.dll") ? "on" : "off");
    logger::debug("[PairTrace] #{} Grapple={} partnerLossBlend={:.3f}; order is callback/poll observation order",
        a_ticket.request, REX::W32::GetModuleHandleW(L"FH_Grapple_Plugin.dll") ? "on" : "off", lossBlend);
    return true;
}

Status ReadStatus() {
    std::lock_guard lock(mutex);
    return status;
}
bool BeginNormalExitMovement(Ticket value, RE::ActorHandle player, int action) {
    {
        std::lock_guard lock(mutex);
        if (status.ticket != value || request.actionIndex != action || status.exitMovementReturned ||
            status.rootHandoffSuppressed || featuresReleased || Participant(player) < 0 || !status.initiator.entered ||
            !status.target.entered ||
            (status.phase != Phase::kPlaying &&
                !(status.phase == Phase::kStopping &&
                    (status.lastEnd == EndReason::kCompleted || status.lastEnd == EndReason::kParticipantUnavailable))))
            return false;
        status.exitMovementReturned = true;
    }
    PairFeatures::Suspend(value);
    const bool released = PairFeatures::BeginMovementRecovery(value, player);
    if (released) Carry::ReleaseMovementControl(value, player);
    const bool loss = ReadStatus().lastEnd == EndReason::kParticipantUnavailable;
    logger::debug("[ExitBlendMove] pair=#{} movement={} preserveBlend={:.3f}{}", value.request, released,
        loss ? Actions::Spec(action).paired.lossBlend : Actions::Spec(action).exitBlend, loss ? " (partner lost)" : "");
    return released;
}
bool TryGetTicket(RE::ActorHandle a_actor, Ticket& a_ticket) {
    std::lock_guard lock(mutex);
    if (status.phase == Phase::kIdle || Participant(a_actor) < 0) return false;
    a_ticket = status.ticket;
    return true;
}
bool TryGetParticipant(const RE::hkbContext& a_context, RE::ActorHandle& a_actor, Ticket& a_ticket) {
    if (!a_context.character) return false;
    std::lock_guard lock(mutex);
    if (status.phase == Phase::kIdle) return false;
    const auto identity = reinterpret_cast<std::uintptr_t>(a_context.character);
    for (int i = 0; i < 2; ++i)
        for (const auto id : characters[i])
            if (id == identity) {
                a_actor = i == 0 ? status.initiator.actor : status.target.actor;
                a_ticket = status.ticket;
                return true;
            }
    return false;
}

void ObserveSyncActivate(Ticket a_ticket, RE::ActorHandle a_actor, const RE::hkbContext& a_context,
    const RE::BSSynchronizedClipGenerator& a_generator) {
    std::lock_guard lock(mutex);
    if (!(a_ticket == status.ticket) || status.phase == Phase::kIdle) return;
    const int i = Participant(a_actor);
    if (i < 0) return;
    const char* expected = expectedNames[i].c_str();
    const char* name = a_generator.name.c_str();
    if (!name || std::strcmp(name, expected) != 0 || a_generator.leadCharacter != (i == 1)) return;
    auto* clip = a_generator.clipGenerator;
    auto* control = clip ? clip->animationControl.get() : nullptr;
    auto* animation = control && control->binding ? control->binding->animation.get() : nullptr;
    const bool ready = animation && std::isfinite(animation->duration) && animation->duration > 0.0f;
    if (i == 0 && ready && !deactivated[i]) movementRecovery.ObserveClock(clip->localTime);
    if (observed[i] && (!ready || loaded[i])) {
        if (ready && !deactivated[i] && sampleCount[i] < 64 && std::isfinite(clip->localTime) &&
            (lastSampleTime[i] < 0.0f || clip->localTime >= lastSampleTime[i] + 0.1f)) {
            Observation sample;
            sample.participant = i;
            sample.sample = true;
            CaptureClock(sample, a_generator);
            PushTrace(sample);
            lastSampleTime[i] = clip->localTime;
            ++sampleCount[i];
        }
        return;
    }
    observed[i] = true;
    loaded[i] = ready;
    Observation o;
    o.participant = i;
    o.loaded = ready;
    o.duration = ready ? animation->duration : 0.0f;
    CaptureClock(o, a_generator);
    o.character = reinterpret_cast<std::uintptr_t>(a_context.character);
    o.generator = reinterpret_cast<std::uintptr_t>(&a_generator);
    o.clipIndex = clip ? static_cast<int>(clip->animationBindingIndex) : -1;
    o.syncIndex = a_generator.synchronizedAnimationBindingIndex;
    if (clip && clip->animationName.c_str())
        strncpy_s(o.file.data(), o.file.size(), clip->animationName.c_str(), _TRUNCATE);
    auto* setup = a_context.character ? a_context.character->setup.get() : nullptr;
    auto* data = setup ? setup->data.get() : nullptr;
    auto* strings = data ? data->stringData.get() : nullptr;
    if (strings && strings->animationNames._data && o.clipIndex >= 0 && o.clipIndex < strings->animationNames._size) {
        const char* selected = strings->animationNames._data[o.clipIndex].c_str();
        if (selected) strncpy_s(o.bindingFile.data(), o.bindingFile.size(), selected, _TRUNCATE);
    }
    Push(o);
}
void ObserveSyncDeactivate(Ticket a_ticket, RE::ActorHandle a_actor, const RE::hkbContext&,
    const RE::BSSynchronizedClipGenerator& a_generator) {
    std::lock_guard lock(mutex);
    if (!(a_ticket == status.ticket) || status.phase == Phase::kIdle) return;
    const int i = Participant(a_actor);
    if (i < 0 || !observed[i] || deactivated[i]) return;
    const char* expected = expectedNames[i].c_str();
    if (!a_generator.name.c_str() || std::strcmp(a_generator.name.c_str(), expected) != 0) return;
    deactivated[i] = true;
    Observation o;
    o.participant = i;
    o.exit = true;
    CaptureClock(o, a_generator);
    Push(o);
}
void ObserveEvent(Ticket a_ticket, RE::ActorHandle a_actor, const RE::BSFixedString& a_event) {
    std::lock_guard lock(mutex);
    if (!(a_ticket == status.ticket) || status.phase == Phase::kIdle) return;
    if (Participant(a_actor) == 0 && !recoveryEvent.empty() && a_event == recoveryEvent)
        movementRecovery.ObserveEvent();
    const bool ending = (!recoveryExitEvent.empty() && a_event == recoveryExitEvent) ||
                        (!customEndEvent.empty() && a_event == customEndEvent) || a_event == Stop() ||
                        a_event == End() || a_event == NpcStop() || a_event == NpcEnd() || a_event == TargetEnd();
    // Capture exit intent before queued lifecycle events reach the main update,
    // including pairs that use an unregistered/default blend effect.
    if (ending) Carry::ObserveEndEvent(a_ticket, a_actor);
    const std::string_view name = a_event.c_str() ? a_event.c_str() : "";
    if (!ending && name.find("Grapple") == name.npos && name.find("ragdoll") == name.npos &&
        name.find("Ragdoll") == name.npos && name.find("death") == name.npos && name.find("Death") == name.npos &&
        name.find("Kill") == name.npos && name.find("getUp") == name.npos && name.find("PartnerLossExit") == name.npos)
        return;
    const int i = Participant(a_actor);
    if (i < 0) return;
    Observation o;
    o.participant = i;
    o.endEvent = ending;
    o.event = true;
    strncpy_s(o.eventName.data(), o.eventName.size(), name.data(), _TRUNCATE);
    if (ending)
        Push(o);
    else
        PushTrace(o);
}
void Cancel(Ticket a_ticket) {
    if (ReadStatus().ticket == a_ticket) BeginStop(EndReason::kCancelled);
}
bool BeginOrdinaryHandoff(Ticket ticket, RE::ActorHandle actor, int action, int partnerAction) {
    const auto validAction = [](int index) {
        return index >= 0 && index < Actions::Count() && !Actions::Spec(index).paired.enabled;
    };
    if (!validAction(action) || (partnerAction != -1 && !validAction(partnerAction))) return false;
    const auto copy = ReadStatus();
    auto self = copy.initiator.actor.get(), target = copy.target.actor.get();
    if (!self || !target || !self->Is3DLoaded() || !target->Is3DLoaded() || self->IsDead() || target->IsDead() ||
        self->IsInRagdollState() || target->IsInRagdollState())
        return false;
    {
        std::lock_guard lock(mutex);
        const int role = Participant(actor);
        if (!(status.ticket == ticket) || status.phase != Phase::kPlaying || role < 0 || !status.initiator.entered ||
            !status.target.entered || deactivated[0] || deactivated[1] || featuresReleased)
            return false;
        handoff = {};
        handoff.destination[role] = action;
        handoff.destination[1 - role] = partnerAction;
        handoff.open = true;
        featuresReleased = true;
    }
    // Stop the old generators, then release feature writes before either new
    // runner acquires them. Keep observations and the native camera tracking.
    {
        std::lock_guard lock(mutex);
        status.rootHandoffSuppressed = true;
    }
    BeginStop(EndReason::kCancelled);
    PairFeatures::End(ticket, true, true);
    logger::debug("[PairHandoff] #{} opened actor={:08X} action={} partnerAction={}; no idle wait", ticket.request,
        actor.native_handle(), action, partnerAction);
    return true;
}
int RetiringHandoffDestination(const RE::hkbContext& context, RE::ActorHandle& actor) {
    if (!context.character) return -1;
    std::lock_guard lock(mutex);
    if (status.phase != Phase::kStopping || status.lastEnd != EndReason::kCancelled || !featuresReleased) return -1;
    const auto identity = reinterpret_cast<std::uintptr_t>(context.character);
    for (int role = 0; role < 2; ++role)
        for (const auto id : characters[role])
            if (id == identity) {
                actor = role == 0 ? status.initiator.actor : status.target.actor;
                return handoff.ProtectedDestination(role);
            }
    return -1;
}
bool CanEnterHandoff(RE::ActorHandle actor, int action) {
    std::lock_guard lock(mutex);
    return status.phase == Phase::kStopping && status.lastEnd == EndReason::kCancelled &&
           handoff.CanEnter(Participant(actor), action);
}
void CompleteOrdinaryHandoff(Ticket ticket, RE::ActorHandle actor, bool actorStarted, bool partnerStarted) {
    {
        std::lock_guard lock(mutex);
        if (!(status.ticket == ticket) || !handoff.open) return;
        handoff.Close(Participant(actor), actorStarted, partnerStarted);
    }
    if (!actorStarted) PairFeatures::AbortHandoffCamera(ticket);
    logger::debug("[PairHandoff] #{} closed started={}/{}; old sync exit observation retained", ticket.request,
        actorStarted, partnerStarted);
}
void ResetOnLoad() {
    std::lock_guard lock(mutex);
    if (status.phase != Phase::kIdle) {
        logger::debug("[PairB] #{} load reset: phase={} entered={}/{} exited={}/{} generation={} -> {}",
            status.ticket.request, static_cast<int>(status.phase), status.initiator.entered, status.target.entered,
            status.initiator.exited, status.target.exited, generation, generation + 1);
    }
    ++generation;
    status = {};
    status.ticket.generation = generation;
    recentExits = {};
    yawLease.Release();  // TDM keeps a lease across loads: the old save's pair returns it
    yawOwner = 0;
    movementRecovery = {};
    recoveryEvent = RE::BSFixedString();
    recoveryExitEvent = RE::BSFixedString();
    status.lastEnd = EndReason::kGameLoad;
    characters = {};
    pendingCount = 0;
    tracePendingCount = 0;
    overflow = false;
    request = {};
    initial = {};
    observed = {};
    loaded = {};
    deactivated = {};
    traceOrder = 0;
    lastSampleTime = {-1.0f, -1.0f};
    sampleCount = {};
    actorTrace = {};
    actorTraceSeen = {};
    nextPlacementTrace = 0;
    handoff = {};
    featuresReleased = false;
    lossBlend = -1.0f;
    lossBlendRequested = false;
    lossExitEvent = RE::BSFixedString();
}

void Update(float a_gameDelta) {
    if (!std::isfinite(a_gameDelta) || a_gameDelta < 0.0f) return;
    {
        std::lock_guard lock(mutex);
        for (auto& recent : recentExits) recent.exit.age += a_gameDelta;
    }
    auto copy = ReadStatus();
    if (copy.phase == Phase::kIdle) return;
    auto self = copy.initiator.actor.get(), target = copy.target.actor.get();
    TraceActor(0, self.get());
    TraceActor(1, target.get());
    if (copy.phase == Phase::kPlaying && self && target && self->Is3DLoaded() && target->Is3DLoaded() &&
        (self->IsDead() || target->IsDead() || self->IsInRagdollState() || target->IsInRagdollState())) {
        BeginStop(EndReason::kParticipantUnavailable);
    }
    if (!self || !target || !self->Is3DLoaded() || !target->Is3DLoaded() || self->IsDead() || target->IsDead()) {
        if (copy.phase != Phase::kStopping) BeginStop(EndReason::kParticipantUnavailable);
    }
    if (copy.phase == Phase::kPendingStart && ReadStatus().phase == Phase::kPendingStart) {
        const auto steering = Carry::PrepareStart(copy.ticket);
        if (steering == Carry::StartGate::kWaiting) {
            // This early return precedes the normal phase clock below.
            float elapsed = 0;
            {
                std::lock_guard lock(mutex);
                status.phaseElapsed += a_gameDelta;
                elapsed = status.phaseElapsed;
            }
            if (elapsed >= request.startTimeout) BeginStop(EndReason::kStartTimeout);
            return;
        }
        if (steering == Carry::StartGate::kFailed) {
            BeginStop(EndReason::kRejected);
            return;
        }
        auto* idle = RE::TESForm::LookupByID<RE::TESIdleForm>(request.idleForm);
        auto* process = self ? self->GetActorRuntimeData().currentProcess : nullptr;
        {
            std::lock_guard lock(mutex);
            status.phase = Phase::kAwaitingBoth;
            status.phaseElapsed = 0.0f;
        }
        // Condition evaluation for the log only: the arguments run even when debug logging is off.
        if (FrameworkSettings::Get().debug && self && target && idle) {
            auto* data = RE::TESDataHandler::GetSingleton();
            auto* reference = data ? data->LookupForm<RE::TESIdleForm>(0xBDE2, "Dawnguard.esm") : nullptr;
            logger::debug("[PairB] #{} idle eligibility injected={} native={} ownConditions={} parent={:08X}",
                copy.ticket.request, self->CanUseIdle(idle), reference && self->CanUseIdle(reference),
                idle->conditions.IsTrue(self.get(), target.get()),
                idle->parentIdle ? idle->parentIdle->GetFormID() : 0);
        }
        const bool accepted = [&] {
            for (int role = 0; role < 2; ++role) {
                auto* actor = role == 0 ? self.get() : target.get();
                if (!actor || !actor->Is3DLoaded() || actor->IsDead() || actor->IsInRagdollState()) continue;
                auto* actorState = actor->AsActorState();
                if (!actorState || actorState->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal) continue;
                const auto before = actorState->GetFlyState();
                actorState->actorState1.flyState = RE::FLY_STATE::kNone;
                auto* controller = actor->GetCharController();
                const bool air =
                    controller && (controller->context.currentState == RE::hkpCharacterStateType::kInAir ||
                                      controller->context.currentState == RE::hkpCharacterStateType::kJumping);
                bool inherited = false;
                {
                    std::lock_guard lock(mutex);
                    const auto handle = actor->GetHandle();
                    for (const auto& recent : recentExits)
                        inherited = inherited || PairStartAir::Inherited(air, recent.actor == handle, recent.exit);
                    auto& participant = role == 0 ? status.initiator : status.target;
                    participant.startGrounded = !air || inherited;
                    participant.inheritedAir = inherited;
                }
                if (air) controller->SetLinearVelocityImpl(RE::hkVector4(0, 0, 0, 0));
                logger::debug("[PairAir] actor={:08X} prepare fly={} air={} inherited={} velocityCleared={}",
                    actor->GetFormID(), static_cast<int>(before), air, inherited, air);
            }
            TracePlacement(copy.ticket.request, "before-native-start", 0, self.get(), target.get());
            PairedStartHooks::Attempt trace(
                copy.ticket.request, request.initiator, request.target, request.actionIndex);
            return process && idle && target &&
                   process->SetupSpecialIdle(
                       self.get(), RE::DEFAULT_OBJECT::kActionIdle, idle, true, true, target.get());
        }();
        TracePlacement(copy.ticket.request, "after-native-start", 0, self.get(), target.get());
        {
            std::lock_guard lock(mutex);
            status.idleCallAccepted = accepted;
        }
        logger::debug("[PairB] #{} SetupSpecialIdle={}", copy.ticket.request, accepted);
        if (!accepted) BeginStop(EndReason::kRejected);
    }
    bool lost;
    {
        std::lock_guard lock(mutex);
        std::size_t lifeIndex = 0, traceIndex = 0;
        while (lifeIndex < pendingCount || traceIndex < tracePendingCount) {
            const bool useLife =
                lifeIndex < pendingCount &&
                (traceIndex == tracePendingCount || pending[lifeIndex].order < tracePending[traceIndex].order);
            const auto& o = useLife ? pending[lifeIndex++] : tracePending[traceIndex++];
            logger::debug(
                "[PairTrace] #{} order={} role={} kind={} local={:.3f} control={:.3f} duration={:.3f} scene={:X} event='{}'",
                status.ticket.request, o.order, o.participant,
                o.event    ? "event"
                : o.exit   ? "pre-deactivate"
                : o.sample ? "sample"
                           : "activate-ready",
                o.localTime, o.controlTime, o.duration, o.scene, o.eventName.data());
            if (o.sample || (o.event && !o.endEvent)) continue;
            auto& part = o.participant == 0 ? status.initiator : status.target;
            if (o.endEvent)
                part.endEventSeen = true;
            else if (o.exit)
                part.exited = true;
            else {
                part.entered = part.entered || o.loaded;
                part.clipDuration = o.duration;
            }
            logger::debug(
                "[PairB] #{} role={} exit={} endEvent={} loaded={} duration={:.3f} character={:X} gen={:X} clip={} sync={} file='{}' bindingFile='{}'",
                status.ticket.request, o.participant, o.exit, o.endEvent, o.loaded, o.duration, o.character,
                o.generator, o.clipIndex, o.syncIndex, o.file.data(), o.bindingFile.data());
        }
        pendingCount = 0;
        tracePendingCount = 0;
        lost = overflow;
        overflow = false;
        status.phaseElapsed += a_gameDelta;
        if (status.phase == Phase::kAwaitingBoth && status.initiator.entered && status.target.entered) {
            status.phase = Phase::kPlaying;
            status.phaseElapsed = 0.0f;
            logger::debug("[PairB] #{} 양쪽 로드된 클립 진입 확인", status.ticket.request);
        }
    }
    if (lost) BeginStop(EndReason::kCancelled);
    copy = ReadStatus();
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* ui = RE::UI::GetSingleton();
    const bool moving = player && player->Is3DLoaded() && ui && !ui->GameIsPaused() &&
                        !ui->IsMenuOpen(RE::Console::MENU_NAME) && (self.get() == player || target.get() == player) &&
                        InputSink::Get()->AnyMoveInput();
    bool recover = false;
    {
        std::lock_guard lock(mutex);
        recover = movementRecovery.Consume(status.phase == Phase::kPlaying && status.initiator.entered &&
                                               status.target.entered && !deactivated[0] && !deactivated[1] &&
                                               !status.initiator.endEventSeen && !status.target.endEventSeen,
            moving);
    }
    if (recover) {
        // Sample before either exit transition changes the authored root pose.
        // These calls must remain outside the session mutex.
        Carry::Sample();
        Carry::ObserveEndEvent(copy.ticket, copy.initiator.actor);
        Carry::Sample();
        logger::info("[PairMoveRecovery] #{} movement exit; current relative carry pose captured", copy.ticket.request);
        BeginStop(EndReason::kMovementRecovery);
        copy = ReadStatus();
    }
    const auto gs = ReadGraph(self.get()), gt = ReadGraph(target.get());
    if (copy.phase == Phase::kPlaying && nextPlacementTrace <= 0.5f && copy.phaseElapsed >= nextPlacementTrace) {
        TracePlacement(copy.ticket.request, "early-playing", copy.phaseElapsed, self.get(), target.get());
        nextPlacementTrace += 0.1f;
    }
    const bool selfUnavailable = !self || !self->Is3DLoaded();
    const bool targetUnavailable = !target || !target->Is3DLoaded();
    {
        std::lock_guard lock(mutex);
        status.initiator.graphStateRestored = PairedCleanup::IsReleased(initial[0], gs, handoff.started[0]);
        status.target.graphStateRestored = PairedCleanup::IsReleased(initial[1], gt, handoff.started[1]);
        status.initiator.graphUnavailable = selfUnavailable;
        status.target.graphUnavailable = targetUnavailable;
    }
    if (copy.phase == Phase::kAwaitingBoth && copy.phaseElapsed >= request.startTimeout)
        BeginStop(EndReason::kStartTimeout);
    else if (copy.phase == Phase::kPlaying) {
        if (copy.initiator.exited && copy.target.exited)
            BeginStop(EndReason::kCompleted);
        else if (copy.phaseElapsed >= request.playbackTimeout)
            BeginStop(EndReason::kPlaybackTimeout);
    }
    copy = ReadStatus();
    if (copy.phase == Phase::kStopping) {
        const bool participantLoss = copy.lastEnd == EndReason::kParticipantUnavailable;
        const auto finished = [participantLoss](const ParticipantStatus& part) {
            return PairedCleanup::IsParticipantFinished(
                part.entered, part.exited, part.graphStateRestored, part.graphUnavailable, participantLoss);
        };
        if (finished(copy.initiator) && finished(copy.target)) {
            if ((gs.valid && initial[0].driven && !gs.driven) || (gt.valid && initial[1].driven && !gt.driven)) {
                logger::debug("[PairB] #{} prior animation-driven control released: initial={}/{} current={}/{}",
                    copy.ticket.request, initial[0].driven, initial[1].driven, gs.driven, gt.driven);
            }
            Finish(copy.lastEnd);
        } else if (copy.phaseElapsed >= request.stopTimeout) {
            logger::warn(
                "[PairB] 정리 제한 초과: 최초사유={} valid={}/{} self synced/pitch/driven={}/{}/{} target={}/{}/{} (강제 bool 변경 안 함)",
                static_cast<int>(copy.lastEnd), gs.valid, gt.valid, gs.synced, gs.pitch, gs.driven, gt.synced, gt.pitch,
                gt.driven);
            Finish(EndReason::kStopTimeout);
        }
    }
}
}  // namespace AnimationInjector::PairedSession
