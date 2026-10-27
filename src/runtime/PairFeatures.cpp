#include "runtime/PairFeatures.h"
#include "runtime/EventEffects.h"
#include "runtime/ActionRegistry.h"
#include "runtime/ActionRunner.h"
#include "runtime/PairMotionMath.h"
#include "runtime/ActionLinks.h"
#include "runtime/Qte.h"
#include "runtime/PairSpatial.h"
#include "runtime/CameraRecovery.h"
#include "runtime/PairEventRoles.h"
#include "Settings.h"
#include "runtime/DrivenFlagOwnership.h"
#include "runtime/LifecyclePolicy.h"
#include "runtime/InputSink.h"
#include "runtime/RecoveryLocomotion.h"
#include "runtime/GetUpRecovery.h"
#include "runtime/WeaponCull.h"
#include "runtime/ViewGuard.h"
#include "core/NativePairAlignment.h"
#include <mutex>
#include <numbers>

namespace AnimationInjector::PairFeatures {
namespace {
std::recursive_mutex mutex;
std::mutex ingressMutex;
struct Incoming {
    RE::ActorHandle actor;
    RE::BSFixedString event;
    std::uint64_t epoch;
};
std::atomic<std::uint64_t> ingressEpoch{0};
std::atomic<std::uint32_t> cameraActor{0};
std::atomic<std::uint32_t> cameraPartner{0};
std::vector<Incoming> incoming;
std::vector<std::pair<RE::ActorHandle, RE::ActorHandle>> hits;
using Ticket = PairedSession::Ticket;
// A clip-time effect waiting for its delay, with the pair roles it reaches (bit 0 initiator, 1 receiver).
struct Job {
    EventFunctionSpec function;
    unsigned targets;
    float remaining;
};
struct Motion {
    std::uintptr_t animation = 0;
    std::vector<PairMotionMath::Key> translation, rotation;
    PairMotionMath::Key previous;
    float previousYaw = 0, time = 0, previousTime = 0;
    PairMotionMath::TranslationBasis basis;
    bool active = false, fresh = false, moved = false;
    unsigned placementSamples = 0;
};
struct Role {
    RE::ActorHandle actor;
    Motion motion;
    bool initialNoSim = false;
    bool initialNotPushable = false, physicsReleased = false;
    bool movementTransferred = false, movementRecovering = false, recoveryDrivenOwned = false;
    bool inputGateReported = false;
    bool motionGateReported = false;
    RecoveryLocomotion recoveryLocomotion;
    RE::NiPointer<RE::NiAVObject> startScene;
    RE::NiPointer<RE::bhkCharacterController> startController;
};
std::array<Role, 2> roles;
// Roles this pair released to native physics, until the engine finishes their get-up.
struct GetUpWatch {
    RE::ActorHandle actor;
    std::uint64_t request;
    GetUpRecovery::Watch state;
    int knock;
};
std::vector<GetUpWatch> getUpWatches;
struct GraphRequest {
    RE::ActorHandle actor;
    std::uint64_t request;
    bool ragdoll;
    int attempt;
    float life;
};
const RE::BSFixedString& GetUpBegin() {
    static RE::BSFixedString name("GetUpBegin");
    return name;
}
const RE::BSFixedString& RagdollInstant() {
    static RE::BSFixedString name("RagdollInstant");
    return name;
}
int KnockOf(RE::Actor* actor) {
    auto* state = actor ? actor->AsActorState() : nullptr;
    return state ? static_cast<int>(state->GetKnockState()) : GetUpRecovery::kKnockNormal;
}
void WatchGetUp(RE::ActorHandle handle, std::uint64_t request) {
    auto actor = handle.get();
    if (!actor || !actor->Is3DLoaded() || actor->IsDead()) return;
    const int knock = KnockOf(actor.get());
    if (!actor->IsInRagdollState() && knock == GetUpRecovery::kKnockNormal) return;
    std::erase_if(getUpWatches, [&](const GetUpWatch& watch) { return watch.actor == handle; });
    if (getUpWatches.size() >= 8) return;
    getUpWatches.push_back({handle, request, {}, knock});
    logger::debug("[PairGetUp] #{} watch actor={:08X} knock={} ragdoll={}", request, actor->GetFormID(), knock,
        actor->IsInRagdollState());
}
// Graph events are returned to the caller and sent after the feature lock is released.
void UpdateGetUpWatches(float delta, std::vector<GraphRequest>& requests) {
    using GetUpRecovery::Step;
    for (auto& watch : getUpWatches) {
        auto actor = watch.actor.get();
        GetUpRecovery::Sample sample;
        if (actor && actor->Is3DLoaded()) {
            auto* state = actor->AsActorState();
            sample.present = true;
            sample.dead = actor->IsDead();
            sample.knock = KnockOf(actor.get());
            sample.held = actor->GetActorRuntimeData().boolBits.all(RE::Actor::BOOL_BITS::kParalyzed) ||
                          (state && state->IsBleedingOut());
        }
        if (sample.present && sample.knock != watch.knock) {
            logger::debug("[PairGetUp] #{} actor={:08X} knock {}->{} t={:.2f}", watch.request, actor->GetFormID(),
                watch.knock, sample.knock, watch.state.life);
            watch.knock = sample.knock;
        }
        switch (GetUpRecovery::Advance(watch.state, sample, delta)) {
        case Step::kNone: break;
        case Step::kRequestGetUp:
            requests.push_back({watch.actor, watch.request, false, watch.state.attempts, watch.state.life});
            break;
        case Step::kEnterRagdoll:
            requests.push_back({watch.actor, watch.request, true, watch.state.attempts, watch.state.life});
            break;
        case Step::kRecovered:
            if (watch.state.attempts > 0)
                logger::info("[PairGetUp] #{} actor={:08X} get-up resumed knock={} t={:.2f} attempts={}", watch.request,
                    actor->GetFormID(), sample.knock, watch.state.life, watch.state.attempts);
            else
                logger::debug("[PairGetUp] #{} actor={:08X} native get-up knock={} t={:.2f}", watch.request,
                    actor->GetFormID(), sample.knock, watch.state.life);
            watch.actor = {};
            break;
        case Step::kGone: watch.actor = {}; break;
        case Step::kGiveUp:
            logger::warn("[PairGetUp] #{} actor={:08X} still knock={} after {:.2f}s attempts={}; watch released",
                watch.request, actor ? actor->GetFormID() : 0, sample.knock, watch.state.life, watch.state.attempts);
            watch.actor = {};
            break;
        }
    }
    std::erase_if(getUpWatches, [](const GetUpWatch& watch) { return !watch.actor; });
}
void SendGraphRequests(const std::vector<GraphRequest>& requests) {
    for (const auto& request : requests) {
        auto actor = request.actor.get();
        if (!actor || !actor->Is3DLoaded()) continue;
        const auto& event = request.ragdoll ? RagdollInstant() : GetUpBegin();
        const bool sent = actor->NotifyAnimationGraph(event);
        logger::info(
            "[PairGetUp] #{} actor={:08X} get-up stalled in kQueued: sent '{}' attempt={} t={:.2f} accepted={}",
            request.request, actor->GetFormID(), event.c_str(), request.attempt, request.life, sent);
    }
}
std::array<RE::BSFixedString, 2> expected;
Ticket ticket;
int actionIndex = -1;
bool suspended = false;
bool active = false, motionEnabled = false, cancel = false, hitCancel = false;
float elapsed = 0;
std::vector<bool> timedFired;  // per clip-time effect of the action (Spec().pairFeatures.timed)
std::vector<Job> jobs;
struct Camera {
    bool owned = false, captured = false, restoreOnEnd = true;
    bool deferredEnd = false;
    // An ordinary action holds the camera (0.12.260): its index while it plays; its timeout waits for its end.
    int ownerAction = -1;
    bool actionRunning = false;
    float targetZoom = 0, currentZoom = 0, timeout = 10, remaining = 0;
    RE::BSFixedString reset;
    RE::ActorHandle player;
    RE::ActorHandle partner;
    CameraRecovery recovery;
    Ticket ownerTicket;
} camera;
void EventOwner(RE::ActorHandle actor, const RE::BSFixedString& event, bool owned) {
    auto sender = actor.get();
    if (!sender || !event.c_str() || !*event.c_str()) return;
    if (auto* source = SKSE::GetModCallbackEventSource()) {
        static RE::BSFixedString name("AnimationInjector_EventOwner");
        SKSE::ModCallbackEvent notification{name, event, owned ? 1.0f : 0.0f, sender.get()};
        source->SendEvent(&notification);
    }
}
RE::ThirdPersonState* Third(RE::PlayerCamera* pc, RE::CameraStates::CameraState state) {
    return pc ? skyrim_cast<RE::ThirdPersonState*>(pc->GetRuntimeData().cameraStates[state].get()) : nullptr;
}
void ClearCameraOverrides(RE::PlayerCamera* pc, bool zoom) {
    if (pc) {
        for (auto state : {RE::CameraStates::kThirdPerson, RE::CameraStates::kAnimated}) {
            if (auto* third = Third(pc, state)) {
                third->toggleAnimCam = false;
                third->animatedBoneName = RE::BSFixedString();
                if (zoom && camera.captured && state == RE::CameraStates::kThirdPerson) {
                    third->targetZoomOffset = camera.targetZoom;
                    third->currentZoomOffset = camera.currentZoom;
                }
            }
        }
    }
}
void ReleaseCamera() {
    EventOwner(camera.player, camera.reset, false);
    EventOwner(camera.partner, camera.reset, false);
    camera = {};
    cameraActor.store(0);
    cameraPartner.store(0);
}
bool CameraReady(RE::PlayerCamera* pc, RE::Actor* player) {
    if (!pc || !player || !player->Is3DLoaded() || player->IsDead() || player->IsInRagdollState()) return false;
    auto* state = player->AsActorState();
    if (!state || state->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal) return false;
    if (!pc->currentState) return false;
    // Do not take the camera away from furniture, mounts, free camera or killcams.
    const auto id = pc->currentState->id;
    return id == RE::CameraStates::kThirdPerson || id == RE::CameraStates::kFirstPerson ||
           id == RE::CameraStates::kAnimated || id == RE::CameraStates::kBleedout ||
           id == RE::CameraStates::kPCTransition;
}
void UpdateCameraRecovery(float delta) {
    if (!camera.owned || !camera.recovery.pending) return;
    auto player = camera.player.get();
    auto* pc = RE::PlayerCamera::GetSingleton();
    const bool ready = CameraReady(pc, player.get());
    bool restored = ready && (pc->IsInThirdPerson() || pc->IsInFirstPerson());
    for (auto state : {RE::CameraStates::kThirdPerson, RE::CameraStates::kAnimated})
        if (auto* third = Third(pc, state))
            restored = restored && !third->toggleAnimCam && third->animatedBoneName.empty();
    const bool wasInterrupted = camera.recovery.interrupted;
    const auto step = camera.recovery.Update(delta, !ready, restored);
    if (!wasInterrupted && camera.recovery.interrupted)
        logger::debug("[PairCamera] recovery deferred: ragdoll={} knock={} state={}",
            player && player->IsInRagdollState(),
            player && player->AsActorState() ? static_cast<int>(player->AsActorState()->GetKnockState()) : -1,
            pc && pc->currentState ? static_cast<int>(pc->currentState->id) : -1);
    switch (step) {
    case CameraRecovery::Step::kApply:
        // Clear the shot before switching, then remove anything restored by the state transition.
        ClearCameraOverrides(pc, true);
        ViewGuard::ExpectCamera(CameraView::kThird);  // ours, not a view change of the player's
        pc->ForceThirdPerson();
        ClearCameraOverrides(pc, true);
        logger::debug("[PairCamera] restore attempt={} state={} elapsed={:.3f}", camera.recovery.attempts,
            pc->currentState ? static_cast<int>(pc->currentState->id) : -1, camera.recovery.age);
        break;
    case CameraRecovery::Step::kComplete:
        logger::debug("[PairCamera] recovery verified: attempts={} afterInterruption={} elapsed={:.3f}",
            camera.recovery.attempts, camera.recovery.interrupted, camera.recovery.age);
        ReleaseCamera();
        break;
    case CameraRecovery::Step::kExpired:
        logger::warn("[PairCamera] recovery abandoned: attempts={} elapsed={:.3f}", camera.recovery.attempts,
            camera.recovery.age);
        ReleaseCamera();
        break;
    default: break;
    }
}
void RestoreCamera() {
    if (!camera.owned || camera.recovery.pending) return;
    camera.recovery.Request();
    ClearCameraOverrides(RE::PlayerCamera::GetSingleton(), false);
    UpdateCameraRecovery(0);
}
int PlayerRole() {
    for (int i = 0; i < 2; ++i)
        if (auto actor = roles[i].actor.get(); actor && actor->IsPlayerRef()) return i;
    return -1;
}
// A clip-time effect for the roles in `mask`, through the event executor (their run scopes own it).
void RunTimed(const EventFunctionSpec& function, unsigned mask) {
    std::vector<RE::ActorHandle> targets;
    for (int i = 0; i < 2; ++i)
        if (mask & (1u << i)) targets.push_back(roles[i].actor);
    auto initiator = roles[0].actor.get();
    auto context = EventEffects::ContextFor(initiator.get(), "pair clip time");
    EventEffects::RunFor(function, context, targets);
}
void Schedule(const EventFunctionSpec& function) {
    const auto mask = PairEventRoles::TimedTargets(function.target, PlayerRole());
    if (function.delay > 0.0f) {
        if (jobs.size() < 128) jobs.push_back({function, mask, function.delay});
    } else
        RunTimed(function, mask);
}
void ApplyMotion(Role& role, RE::Actor* actor) {
    auto& motion = role.motion;
    if (!motion.fresh || role.physicsReleased || actor->IsInRagdollState() || actor->IsDead()) return;
    motion.fresh = false;
    if (motion.time < motion.previousTime - 0.001f) {
        motion.previous = {};
        motion.previousYaw = 0;
    }
    const auto value = PairMotionMath::Sample(motion.translation, motion.time);
    const float yaw = PairMotionMath::Sample(motion.rotation, motion.time).z * std::numbers::pi_v<float> / 180.0f;
    const float x = value.x - motion.previous.x, y = value.y - motion.previous.y, z = value.z - motion.previous.z;
    const float angle = yaw - motion.previousYaw;
    const float appliedRotation = motion.previousYaw;
    motion.previous = value;
    motion.previousYaw = yaw;
    motion.previousTime = motion.time;
    if (std::fabs(x) + std::fabs(y) + std::fabs(z) > 0.0001f)
        if (auto* controller = actor->GetCharController()) {
            // The basis is the heading the start aligns the actor to: a victim the start is still turning would
            // otherwise keep the part-turned heading for the whole pair and drift sideways (2026-10-28: 4.45 degrees
            // off, 13.5 degrees of bearing in half a second).
            float heading = actor->GetAngleZ();
            if (!motion.basis.captured)
                if (const auto goal = NativePairAlignment::GoalYaw(actor)) heading = *goal;
            motion.basis.Observe(std::fabs(x) + std::fabs(y) + std::fabs(z), heading, appliedRotation);
            if (!motion.basis.captured) return;
            const auto position = actor->GetPosition();
            const float s = std::sin(motion.basis.yaw), c = std::cos(motion.basis.yaw);
            const auto target = position + RE::NiPoint3{x * c + y * s, -x * s + y * c, z};
            const float scale = RE::bhkWorld::GetWorldScale();
            if (FrameworkSettings::Get().debug && motion.placementSamples < 3) {
                ++motion.placementSamples;
                RE::hkVector4 physical;
                controller->GetPositionImpl(physical, true);
                const auto& xyz = physical.quad.m128_f32;
                const float inv = RE::bhkWorld::GetWorldScaleInverse();
                logger::debug(
                    "[PairPlacement] #{} first-motion actor={:08X} sample={} local={:.3f} delta=({:.2f},{:.2f},{:.2f}) basis={:.5f} actorPos=({:.2f},{:.2f},{:.2f}) controllerPos=({:.2f},{:.2f},{:.2f}) requested=({:.2f},{:.2f},{:.2f})",
                    ticket.request, actor->GetFormID(), motion.placementSamples, motion.time, x, y, z, motion.basis.yaw,
                    position.x, position.y, position.z, xyz[0] * inv, xyz[1] * inv, xyz[2] * inv, target.x, target.y,
                    target.z);
            }
            controller->SetPositionImpl(
                RE::hkVector4(position.x * scale, position.y * scale, position.z * scale, 0), true, true);
            controller->SetPositionImpl(
                RE::hkVector4(target.x * scale, target.y * scale, target.z * scale, 0), true, false);
            motion.moved = true;
        }
    if (std::fabs(angle) > 0.00001f) {
        auto angles = actor->GetAngle();
        angles.z = std::remainder(angles.z + angle, 2 * std::numbers::pi_v<float>);
        actor->SetAngle(angles);
    }
}
}

void Begin(Ticket value, int action, RE::ActorHandle initiator, RE::ActorHandle target) {
    std::lock_guard lock(mutex);
    camera.deferredEnd = false;
    // A new shot supersedes pending verification; preserve its original zoom only for camera chaining.
    if (camera.owned && camera.recovery.pending) {
        EventOwner(camera.player, camera.reset, false);
        EventOwner(camera.partner, camera.reset, false);
        camera.recovery = {};
        const bool carriesCamera = !Actions::Spec(action).pairFeatures.cameraReset.empty() &&
                                   (initiator == camera.player || target == camera.player);
        if (!carriesCamera) {
            camera = {};
            cameraActor.store(0);
            cameraPartner.store(0);
        }
    }
    ticket = value;
    actionIndex = action;
    active = true;
    elapsed = 0;
    cancel = false;
    ++ingressEpoch;
    roles = {};
    roles[0].actor = initiator;
    roles[1].actor = target;
    {
        std::lock_guard ingress(ingressMutex);
        incoming.clear();
        hits.clear();
    }
    suspended = false;
    jobs.clear();
    const auto& spec = Actions::Spec(action);
    auto starter = initiator.get();
    ActionLinks::Begin(initiator, action, true, starter && starter->IsPlayerRef(), target);
    Qte::Begin(initiator, action, target);
    const auto& features = spec.pairFeatures;
    motionEnabled = true;  // a pair moves by its HKX's AMR annotations, as every clip does (0.12.231)
    hitCancel = features.interruptOnThirdPartyHit;
    expected = {
        RE::BSFixedString(spec.clips[1].enterEvent.c_str()), RE::BSFixedString(spec.clips[0].enterEvent.c_str())};
    for (auto& role : roles)
        if (auto actor = role.actor.get()) {
            role.startScene.reset(actor->Get3D(false));
            role.startController.reset(actor->GetCharController());
            if (auto* controller = actor->GetCharController()) {
                role.initialNoSim = controller->flags.all(RE::CHARACTER_FLAGS::kNoSim);
                role.initialNotPushable = controller->flags.all(RE::CHARACTER_FLAGS::kNotPushable);
            }
        }
    if (!features.cameraReset.empty()) {
        auto first = initiator.get(), second = target.get();
        auto* player = first && first->IsPlayerRef()     ? first.get()
                       : second && second->IsPlayerRef() ? second.get()
                                                         : nullptr;
        if (player) {
            if (camera.owned) {
                EventOwner(camera.player, camera.reset, false);
                EventOwner(camera.partner, camera.reset, false);
            }
            // Preserve the pre-animation zoom across an explicitly chained camera shot.
            if (!camera.owned)
                if (auto* third = Third(RE::PlayerCamera::GetSingleton(), RE::CameraStates::kThirdPerson)) {
                    camera.targetZoom = third->targetZoomOffset;
                    camera.currentZoom = third->currentZoomOffset;
                    camera.captured = true;
                }
            camera.owned = true;
            camera.ownerAction = -1;
            camera.actionRunning = false;
            camera.player = player->GetHandle();
            camera.partner = initiator == camera.player ? target : initiator;
            cameraActor.store(camera.player.native_handle());
            cameraPartner.store(camera.partner.native_handle());
            camera.reset = RE::BSFixedString(features.cameraReset.c_str());
            camera.restoreOnEnd = features.cameraRestoreOnEnd;
            camera.timeout = features.cameraTimeout;
            camera.remaining = features.cameraTimeout;
            camera.ownerTicket = ticket;
            EventOwner(camera.player, camera.reset, true);
            EventOwner(camera.partner, camera.reset, true);
        }
    }
    timedFired.assign(features.timed.size(), false);
    for (std::size_t i = 0; i < features.timed.size(); ++i)
        if (features.timed[i].at <= 0.0f) {
            timedFired[i] = true;
            Schedule(features.timed[i].function);
        }
    logger::debug("[PairFeatures] #{} begin action='{}' motion={} timed={}", ticket.request, spec.id, motionEnabled,
        features.timed.size());
}
static bool RecoveryMovementEligible(const Role& role, RE::Actor* actor) {
    auto* state = actor ? actor->AsActorState() : nullptr;
    auto* controller = actor ? actor->GetCharController() : nullptr;
    bool eligible = actor && actor->IsPlayerRef() && actor->Is3DLoaded() && state && !actor->IsDead() &&
                    !actor->IsInRagdollState() && state->GetKnockState() == RE::KNOCK_STATE_ENUM::kNormal &&
                    !state->IsStaggered() && !state->IsBleedingOut() &&
                    state->GetAttackState() == RE::ATTACK_STATE_ENUM::kNone &&
                    role.startScene.get() == actor->Get3D(false) && controller &&
                    controller == role.startController.get() && !role.initialNoSim;
    return eligible && !Actions::FindRunner(actor, [](int, ActionRunner& r) {
        return r.GetState() != ActionRunner::State::kIdle && r.GetState() != ActionRunner::State::kBlendingOut;
    });
}

void MaintainMovementRecovery() {
    std::lock_guard lock(mutex);
    if (!active || !suspended) return;
    static const RE::BSFixedString driven("bAnimationDriven"), rotation("bAllowRotation");
    static const RE::BSFixedString speed("Speed");
    static const RE::BSFixedString moveStart("moveStart"), moveStop("moveStop");
    for (auto& role : roles) {
        if (!role.movementRecovering) continue;
        auto actor = role.actor.get();
        auto* controller = actor ? actor->GetCharController() : nullptr;
        const bool eligible = RecoveryMovementEligible(role, actor.get());
        MaintainRecoveredDrivenFlag(role.movementRecovering, role.recoveryDrivenOwned, eligible,
            [&](bool value) { return actor->SetGraphVariableBool(driven, value); });
        MaintainRecoveredRotation(
            role.movementRecovering, [&](bool& value) { return actor->GetGraphVariableBool(rotation, value); },
            [&](bool value) { return actor->SetGraphVariableBool(rotation, value); });
        if (!role.movementRecovering) continue;
        // The pair acquired NoSim after Begin. Preserve pre-existing physics owners
        // and every other controller flag; never clear sync/pitch to fake completion.
        controller->flags.reset(RE::CHARACTER_FLAGS::kNoSim);
        // The native movement calculation can now run, but its one-shot action
        // request is still rejected by the sync action filter. Forward only the
        // matching locomotion edge from intent without waiting for acceleration.
        // Never retry successful edges every frame or force an idle on release.
        float locomotionSpeed = 0;
        const bool bridge = role.motionGateReported && AllowRecoveryInput(role.actor);
        const bool readable = bridge && actor->GetGraphVariableFloat(speed, locomotionSpeed);
        const bool moveIntent = bridge && InputSink::Get()->CurrentMoveDir(MoveDir::kNone) != MoveDir::kNone;
        role.recoveryLocomotion.Update(bridge, readable, locomotionSpeed, moveIntent, [&](bool moving) {
            const auto& event = moving ? moveStart : moveStop;
            const bool sent = actor->NotifyAnimationGraph(event);
            if (sent)
                logger::debug("[PairMoveAnimation] #{} event={} speed={:.3f} intent={}; paired exit retained",
                    ticket.request, event.c_str(), locomotionSpeed, moveIntent);
            return sent;
        });
    }
}
bool BeginMovementRecovery(Ticket value, RE::ActorHandle owner) {
    std::lock_guard lock(mutex);
    if (!active || !suspended || ticket != value) return false;
    for (auto& role : roles) {
        if (role.actor != owner) continue;
        if (role.movementTransferred) return role.movementRecovering;
        auto actor = owner.get();
        if (!RecoveryMovementEligible(role, actor.get())) return false;
        role.movementTransferred = role.movementRecovering = role.recoveryDrivenOwned = true;
        role.motion.active = role.motion.fresh = false;
        MaintainMovementRecovery();
        return role.movementRecovering;
    }
    return false;
}
bool MovementRecoveryActive(Ticket value, RE::ActorHandle owner) {
    std::lock_guard lock(mutex);
    if (!active || !suspended || ticket != value) return false;
    for (const auto& role : roles)
        if (role.actor == owner && role.movementRecovering) {
            auto actor = owner.get();
            return RecoveryMovementEligible(role, actor.get());
        }
    return false;
}
bool AllowRecoveryInput(RE::ActorHandle owner) {
    std::lock_guard lock(mutex);
    if (!active || !suspended) return false;
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* ui = RE::UI::GetSingleton();
    auto* controls = RE::PlayerControls::GetSingleton();
    auto* map = RE::ControlMap::GetSingleton();
    if (!player || !player->Is3DLoaded() || owner != player->GetHandle() || !ui || ui->GameIsPaused() ||
        ui->IsMenuOpen(RE::Console::MENU_NAME) || !controls || controls->blockPlayerInput || !map ||
        !map->IsMovementControlsEnabled())
        return false;
    const auto& movement = player->GetActorRuntimeData().movementController;
    if (!movement || !movement->controlsDriven || player->GetPlayerRuntimeData().playerFlags.aiControlledPackage ||
        player->IsOnMount())
        return false;
    for (auto& role : roles)
        if (role.actor == owner && role.movementRecovering && role.movementTransferred &&
            RecoveryMovementEligible(role, player)) {
            if (!role.inputGateReported) {
                role.inputGateReported = true;
                logger::debug(
                    "[PairMoveInput] #{} player input resumed during exit; native sync scene retained", ticket.request);
            }
            return true;
        }
    return false;
}
bool AllowRecoveryMotion(RE::ActorHandle owner) {
    std::lock_guard lock(mutex);
    if (!AllowRecoveryInput(owner)) return false;
    for (auto& role : roles)
        if (role.actor == owner && !role.motionGateReported) {
            role.motionGateReported = true;
            logger::debug(
                "[PairMoveMotion] #{} native input-driven movement resumed; graph sync flag and paired clips retained",
                ticket.request);
        }
    return true;
}
void Suspend(Ticket value) {
    std::lock_guard lock(mutex);
    if (!active || ticket != value) return;
    suspended = true;
    jobs.clear();
}
void End(Ticket value, bool completed, bool handoff) {
    std::lock_guard lock(mutex);
    if (!active || !(value == ticket)) return;
    ActionLinks::End(roles[0].actor, actionIndex);
    ActionLinks::End(roles[1].actor, actionIndex);
    Qte::End(roles[0].actor, actionIndex, roles[1].actor);
    for (int i = 0; i < 2; ++i) {
        auto& role = roles[i];
        if (role.motion.moved || !role.motion.rotation.empty())
            logger::debug("[PairMotion] #{} end role={} time={:.3f} cumulative=({:.2f},{:.2f},{:.2f}) yaw={:.3f}",
                ticket.request, i, role.motion.previousTime, role.motion.previous.x, role.motion.previous.y,
                role.motion.previous.z, role.motion.previousYaw);
        if (auto actor = role.actor.get()) {
            if (actor->Is3DLoaded())
                if (auto* controller = actor->GetCharController()) {
                    const auto before = controller->flags.get();
                    // A ragdoll function transfers this role to native physics.
                    // Reconcile at exit even without AMR translation. Preserve flags
                    // predating this pair, and never modify a chained destination.
                    if (!handoff && !role.movementTransferred) {
                        if ((role.motion.moved || role.physicsReleased) && !role.initialNoSim)
                            controller->flags.reset(RE::CHARACTER_FLAGS::kNoSim);
                        if (role.physicsReleased && !role.initialNotPushable)
                            controller->flags.reset(RE::CHARACTER_FLAGS::kNotPushable);
                    }
                    logger::debug(
                        "[PairPhysics] #{} phase=end actor={:08X} releaseRequested={} moved={} handoff={} initialNoSim={} initialNotPushable={} flags={:08X}->{:08X} ragdoll={}",
                        ticket.request, actor->GetFormID(), role.physicsReleased, role.motion.moved, handoff,
                        role.initialNoSim, role.initialNotPushable, static_cast<unsigned>(before),
                        static_cast<unsigned>(controller->flags.get()), actor->IsInRagdollState());
                }
            // The engine's single get-up request can arrive while this pair still held the graph.
            if (!handoff && role.physicsReleased) WatchGetUp(role.actor, ticket.request);
        }
    }
    if (camera.owned && (!completed || camera.restoreOnEnd)) {
        if (completed && (handoff || ActionLinks::HasPendingTransition(camera.player)))
            camera.deferredEnd = true;
        else
            RestoreCamera();
    }
    active = false;
    jobs.clear();
    timedFired.clear();
    roles = {};
}
void BeginActionCamera(RE::ActorHandle player, int action) {
    if (action < 0 || action >= Actions::Count()) return;
    const auto& features = Actions::Spec(action).pairFeatures;
    auto actor = player.get();
    if (features.cameraReset.empty() || !actor || !actor->IsPlayerRef()) return;
    std::lock_guard lock(mutex);
    if (active) return;  // a pair plays: its own camera rule holds
    if (camera.owned) EventOwner(camera.player, camera.reset, false);
    camera.deferredEnd = false;
    // The zoom before the first shot of a chain is what comes back.
    if (!camera.owned || camera.recovery.pending) {
        camera.recovery = {};
        if (!camera.owned)
            if (auto* third = Third(RE::PlayerCamera::GetSingleton(), RE::CameraStates::kThirdPerson)) {
                camera.targetZoom = third->targetZoomOffset;
                camera.currentZoom = third->currentZoomOffset;
                camera.captured = true;
            }
    }
    camera.owned = true;
    camera.player = player;
    camera.partner = {};
    cameraActor.store(player.native_handle());
    cameraPartner.store(0);
    camera.reset = RE::BSFixedString(features.cameraReset.c_str());
    camera.restoreOnEnd = features.cameraRestoreOnEnd;
    camera.timeout = features.cameraTimeout;
    camera.remaining = features.cameraTimeout;
    camera.ownerTicket = {};
    camera.ownerAction = action;
    camera.actionRunning = true;
    EventOwner(camera.player, camera.reset, true);
    logger::debug("[ActionCamera] begin action='{}' restore={} reset='{}'", Actions::Spec(action).id,
        camera.restoreOnEnd ? "end" : "event", features.cameraReset);
}
void EndActionCamera(RE::ActorHandle player, int action, bool completed) {
    std::lock_guard lock(mutex);
    // Only the action holding it: a chained action that took the camera over keeps it.
    if (!camera.owned || camera.ownerAction != action || camera.player != player) return;
    camera.actionRunning = false;
    if (completed && !camera.restoreOnEnd) return;  // waits for its reset event, or the timeout
    if (completed && ActionLinks::HasPendingTransition(camera.player))
        camera.deferredEnd = true;  // the next action may take it over; Update gives it back otherwise
    else
        RestoreCamera();
}
bool CameraBusy() {
    std::lock_guard lock(mutex);
    return camera.owned;
}
void AbortHandoffCamera(Ticket value) {
    std::lock_guard lock(mutex);
    if (camera.owned && camera.ownerTicket == value) RestoreCamera();
}
static void ProcessEvent(RE::ActorHandle actor, const RE::BSFixedString& event) {
    // Either participant may carry the reset annotation after a role-swapped chain.
    if (camera.owned && actor && (actor == camera.player || actor == camera.partner) && event == camera.reset) {
        camera.remaining = 0;
        logger::debug("[PairCamera] reset event accepted from {} of ticket #{}",
            actor == camera.player ? "player" : "partner", camera.ownerTicket.request);
    }
}
void ObserveEvent(RE::ActorHandle actor, const RE::BSFixedString& event) {
    // Event callbacks must never wait on a lock held while mutating an actor graph.
    const auto id = actor.native_handle();
    if (!id || (id != cameraActor.load() && id != cameraPartner.load())) return;
    const auto epoch = ingressEpoch.load();
    std::lock_guard lock(ingressMutex);
    if (incoming.size() < 256) incoming.push_back({actor, event, epoch});
}
void ObserveSync(Ticket value, RE::ActorHandle actor, const RE::BSSynchronizedClipGenerator& generator, bool exiting) {
    // A missed sample is retried with cumulative motion next frame. Never invert graph locks.
    std::unique_lock lock(mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    if (!active || !(value == ticket)) return;
    const int index = actor == roles[0].actor ? 0 : actor == roles[1].actor ? 1 : -1;
    if (index < 0 || !generator.name.c_str() || std::strcmp(generator.name.c_str(), expected[index].c_str()) != 0)
        return;
    auto* clip = generator.clipGenerator;
    auto* control = clip ? clip->animationControl.get() : nullptr;
    auto* animation = control && control->binding ? control->binding->animation.get() : nullptr;
    if (!animation || !std::isfinite(clip->localTime)) return;
    ActionLinks::ObserveTime(actor, actionIndex, clip->localTime, animation->duration);
    Qte::ObserveTime(actor, actionIndex, clip->localTime);
    if (!motionEnabled) return;
    auto& motion = roles[index].motion;
    const auto identity = reinterpret_cast<std::uintptr_t>(animation);
    if (motion.animation != identity) {
        motion = {};
        motion.animation = identity;
        const char* rawPrefix = generator.syncAnimPrefix.c_str();
        const std::string_view prefix = rawPrefix && *rawPrefix ? rawPrefix : "NPC";
        for (const auto& track : animation->annotationTracks) {
            bool found = false;
            for (const auto& annotation : track.annotations) {
                PairMotionMath::Key key;
                bool rotation = false;
                std::string_view text = annotation.text.c_str() ? annotation.text.c_str() : "";
                if (!text.starts_with(prefix)) continue;
                text.remove_prefix(prefix.size());
                while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
                if (PairMotionMath::Parse(text, annotation.time, key, rotation)) {
                    (rotation ? motion.rotation : motion.translation).push_back(key);
                    found = true;
                }
            }
            if (found) break;
        }
        PairMotionMath::Sort(motion.translation);
        PairMotionMath::Sort(motion.rotation);
        logger::debug("[PairMotion] #{} role={} translation={} rotation={}", ticket.request, index,
            motion.translation.size(), motion.rotation.size());
    }
    motion.time = std::max(0.0f, clip->localTime);
    motion.fresh = true;
    motion.active = !exiting;
}
void Update(float delta) {
    if (!std::isfinite(delta) || delta < 0) return;
    const bool ordinaryActive = Actions::AnyActive();
    Ticket cancelTicket;
    bool shouldCancel = false;
    std::vector<GraphRequest> graphRequests;
    {
        std::lock_guard lock(mutex);
        std::vector<Incoming> events;
        UpdateGetUpWatches(delta, graphRequests);
        std::vector<std::pair<RE::ActorHandle, RE::ActorHandle>> hitEvents;
        {
            std::lock_guard ingress(ingressMutex);
            events.swap(incoming);
            hitEvents.swap(hits);
        }
        if (active && !suspended)
            for (const auto& role : roles) {
                auto actor = role.actor.get();
                if (!actor || !actor->Is3DLoaded() || actor->IsDead() || actor->IsInRagdollState()) {
                    suspended = true;
                    jobs.clear();
                    break;
                }
            }
        for (const auto& event : events)
            if (event.epoch == ingressEpoch.load()) ProcessEvent(event.actor, event.event);
        if (active && hitCancel)
            for (const auto& [victim, attacker] : hitEvents)
                if ((victim == roles[0].actor || victim == roles[1].actor) && attacker && attacker != roles[0].actor &&
                    attacker != roles[1].actor)
                    cancel = true;
        if (camera.owned) {
            auto player = camera.player.get();
            if (!player || !player->Is3DLoaded() || player->IsDead() || player->IsInRagdollState() ||
                (camera.deferredEnd && !active && !ordinaryActive && !ActionLinks::HasPendingTransition(camera.player)))
                RestoreCamera();
            // The reset event's wait starts when the owner has ended (a pair, or the ordinary action).
            if (!active && !camera.actionRunning) camera.remaining -= delta;
            if (camera.remaining <= 0) RestoreCamera();
            UpdateCameraRecovery(delta);
        }
        // Keep survivor motion and camera recovery while gameplay jobs are suspended.
        if (active)
            for (auto& role : roles)
                if (auto actor = role.actor.get(); actor && actor->Is3DLoaded() && !role.movementTransferred)
                    ApplyMotion(role, actor.get());
        if (active && !suspended) {
            elapsed += delta;
            const auto& timed = Actions::Spec(actionIndex).pairFeatures.timed;
            // Remove a job before execution: spell casts can synchronously enqueue events.
            std::vector<Job> due;
            for (auto& job : jobs) {
                job.remaining -= delta;
                if (job.remaining <= 0) due.push_back(job);
            }
            std::erase_if(jobs, [](const Job& job) { return job.remaining <= 0; });
            for (std::size_t i = 0; i < timed.size() && i < timedFired.size(); ++i)
                if (!timedFired[i] && elapsed >= timed[i].at) {
                    timedFired[i] = true;
                    Schedule(timed[i].function);
                }
            for (const auto& job : due) RunTimed(job.function, job.targets);
            shouldCancel = cancel;
            cancel = false;
            cancelTicket = ticket;
        }
    }
    SendGraphRequests(graphRequests);
    MaintainMovementRecovery();
    if (shouldCancel) PairedSession::Cancel(cancelTicket);
}
void ObserveHit(RE::ActorHandle victim, RE::ActorHandle attacker) {
    std::lock_guard lock(ingressMutex);
    if (hits.size() < 64) hits.emplace_back(victim, attacker);
}
bool ReleasePhysics(RE::ActorHandle actor) {
    std::lock_guard lock(mutex);
    if (!active) return false;
    for (auto& role : roles)
        if (role.actor == actor)
            if (auto a = actor.get())
                if (auto* controller = a->GetCharController()) {
                    const auto before = controller->flags.get();
                    if (!role.initialNotPushable) controller->flags.reset(RE::CHARACTER_FLAGS::kNotPushable);
                    if (!role.initialNoSim) {
                        controller->flags.reset(RE::CHARACTER_FLAGS::kNoSim);
                        controller->SetLinearVelocityImpl(RE::hkVector4(0, 0, 0, 0));
                    }
                    role.physicsReleased = true;
                    logger::debug(
                        "[PairPhysics] #{} phase=before-spell actor={:08X} initialNoSim={} initialNotPushable={} flags={:08X}->{:08X}",
                        ticket.request, a->GetFormID(), role.initialNoSim, role.initialNotPushable,
                        static_cast<unsigned>(before), static_cast<unsigned>(controller->flags.get()));
                    return true;
                }
    return false;
}
void RequestCancel(RE::ActorHandle actor) {
    std::lock_guard lock(mutex);
    if (active && !suspended && (actor == roles[0].actor || actor == roles[1].actor)) cancel = true;
}
void ResetOnLoad() {
    std::lock_guard lock(mutex);
    getUpWatches.clear();
    WeaponCull::Reset();  // every hide's nodes belong to the old 3D
    ++ingressEpoch;
    // PreLoadGame ends the old session first. Post-load must never resolve old actor handles.
    // Cancel old-save recovery without resolving old actor handles or forcing a camera state.
    if (camera.owned) ClearCameraOverrides(RE::PlayerCamera::GetSingleton(), true);
    camera = {};
    cameraActor.store(0);
    cameraPartner.store(0);
    active = false;
    roles = {};
    jobs.clear();
    timedFired.clear();
    {
        std::lock_guard ingress(ingressMutex);
        incoming.clear();
        hits.clear();
    }
    ticket = {};
    actionIndex = -1;
}
bool Translation(RE::ActorHandle actor, int action, float time, RE::NiPoint3& output) {
    std::unique_lock lock(mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    if (!active || !motionEnabled || action != actionIndex || !std::isfinite(time)) return false;
    for (auto& role : roles)
        if (role.actor == actor && !role.physicsReleased && !role.motion.translation.empty()) {
            const auto value = PairMotionMath::Sample(role.motion.translation, time);
            output = {value.x, value.y, value.z};
            return true;
        }
    return false;
}
bool Rotation(RE::ActorHandle actor, int action, float time, RE::NiQuaternion& output) {
    std::unique_lock lock(mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    if (!active || !motionEnabled || action != actionIndex || !std::isfinite(time)) return false;
    for (auto& role : roles)
        if (role.actor == actor && !role.physicsReleased && !role.motion.rotation.empty()) {
            const float halfYaw =
                PairMotionMath::Sample(role.motion.rotation, time).z * std::numbers::pi_v<float> / 360.0f;
            output = {std::cos(halfYaw), 0, 0, std::sin(halfYaw)};
            return true;
        }
    return false;
}
}
