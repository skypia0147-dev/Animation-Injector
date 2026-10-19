#include "PCH.h"
#include "runtime/Carry.h"
#include "runtime/ActorImmunity.h"
#include "runtime/CarryOwnership.h"
#include "runtime/CarryContinuity.h"
#include "runtime/CarryPlacement.h"
#include "runtime/CarryHandoff.h"
#include "runtime/CarryDrop.h"
#include "runtime/PairStartControl.h"
#include "runtime/PairStartTransfer.h"
#include "core/CarryAttachment.h"
#include "core/CarryPhysics.h"
#include "core/CarryPoseLock.h"
#include "core/GraphBoneSlot.h"
#include "runtime/CarryHold.h"
#include "runtime/CarryBody.h"
#include "runtime/ActionRegistry.h"
#include "runtime/ActionRunner.h"
#include "runtime/PairedSession.h"
#include "runtime/CarryGround.h"
#include "runtime/CarryWeaponCover.h"
#include "runtime/PrecisionLink.h"
#include "runtime/EventEffects.h"
#include "Settings.h"
#include <atomic>
#include <mutex>

namespace AnimationInjector::Carry {
namespace {
// kPuttingDown: the carrier plays its putdown Action while the target stays attached; the Action's
// CarryRelease moment drops the target as a ragdoll (docs/설계-운반-래그돌-내려놓기.md).
enum class Phase { kIdle, kLifting, kCarrying, kPuttingDown, kSuspended };
struct State {
    int entry = -1;
    Phase phase = Phase::kIdle;
    RE::ActorHandle carrier, target;
    RE::NiPointer<RE::bhkCharacterController> controller;
    CarryAttachment::Lease attachment;
    CarryOwnership::Flags flags;
    CarryOwnership::ContactFlags filterFlags;
    PairStartControl::Lease startControl;
    bool startHanded = false;
    std::uintptr_t characters[2]{};
    PairedSession::Ticket ticket;
    bool aiChanged = false;
    float gravity = 1, age = 0, trace = 0, distance = 0, syncFailureTime = 0, putdownAge = 0;
    RE::NiPoint3 lastCarrier{}, ground{};
    RE::NiTransform syncOffset;
    CarryHandoff::Pose<RE::NiTransform> endPose;
    RE::NiPointer<RE::NiAVObject> sampledCarrierScene, sampledTargetScene;
    RE::NiPointer<RE::NiNode> sampledAnchor;
    RE::NiTransform holdOriginal{}, holdApplied{};
    bool holding = false;
    // Physics target: starts once the native sync has placed the scene (see Attach).
    bool bodyPending = false;
    // A physics safety stop is not final: the bodies restart from the locked pose after bodyRestartIn
    // seconds. A stop within 2 s of the last start doubles the wait (0.3 s up to 5 s); a restart
    // that cannot start the bodies waits the same way, at most maxBodyRetries times in a row.
    float bodyRestartIn = -1, bodyBackoff = 0, bodyAge = 0;
    std::uint32_t bodyRestarts = 0, bodyFailedStarts = 0;
    // Fix moment (CarrySpec::fixAt): the pose is locked and the root follows the carrier bone while the
    // lift still plays. liftEnded is the lift's real end: the arm hold, head tracking and end hold.
    bool fixed = false, liftEnded = false, follows = false;
    RE::NiPointer<RE::NiAVObject> rootOwner;  // graph bone 0: a plain node or a flattened bone tree
    std::int32_t rootEntry = -1;
    float sampleAge = 0;
    RE::FormID lastCarrierCell = 0, lastTargetCell = 0;
    bool hasGround = false;
    RE::FormID targetId = 0;
    // Keep the reference alive through streaming, while every operation still
    // resolves its ActorHandle. This does not retain an unloaded 3D scene.
    RE::NiPointer<RE::TESObjectREFR> targetLifetime;
    CarryContinuity::TransferGate transfer;
    RE::NiPointer<RE::NiAVObject> hiddenScene;
    bool hiddenOwned = false;
};
State state;
struct ContactRecovery {
    RE::ActorHandle target;
    RE::NiPointer<RE::TESObjectREFR> lifetime;
    CarryOwnership::ContactFlags flags;
};
ContactRecovery contactRecovery;
// A carry cancelled while the target is held drops it as a ragdoll once the target's controller,
// contacts and AI are restored (docs/설계-운반-개편.md). The engine owns the get-up.
struct Drop {
    RE::ActorHandle target;
    RE::FormID id = 0;
    CarryDrop::NativeHandoff handoff;
    // A save made while carrying keeps the target at shoulder height; on load it is dropped the same
    // way once its 3D appears (game time, bounded).
    bool fromLoad = false;
    float waited = 0;
};
Drop drop;
// Debug log only: a dropped target from its knockdown to 25 s later — the get-up, and whether it walks
// away (2026-09-28: a target once stood in place after getting up; not reproduced since).
struct DropWatch {
    RE::ActorHandle target;
    RE::FormID id = 0;
    float age = 0, next = 0;
    RE::NiPoint3 last{};
};
DropWatch dropWatch;
void StartDropWatch(RE::Actor* target) {
    if (!FrameworkSettings::Get().debug || !target) return;
    dropWatch = {target->GetHandle(), target->GetFormID(), 0, 0, target->GetPosition()};
}
void TraceDropWatch(float delta) {
    if (!dropWatch.id || !std::isfinite(delta)) return;
    dropWatch.age += std::max(delta, 0.0f);
    if (dropWatch.age < dropWatch.next) return;
    dropWatch.next = dropWatch.age + 0.5f;
    auto target = dropWatch.target.get();
    if (!target || !target->Is3DLoaded() || dropWatch.age > 25.0f) {
        logger::debug("[CarryDropWatch] end target={:08X} t={:.1f}", dropWatch.id, dropWatch.age);
        dropWatch = {};
        return;
    }
    static const RE::BSFixedString driven("bAnimationDriven"), synced("bIsSynced");
    bool drivenValue = false, syncedValue = false;
    const bool drivenRead = target->GetGraphVariableBool(driven, drivenValue);
    const bool syncedRead = target->GetGraphVariableBool(synced, syncedValue);
    auto* actorState = target->AsActorState();
    auto* cc = target->GetCharController();
    const auto* package = target->GetCurrentPackage();
    const auto position = target->GetPosition();
    const float moved = position.GetDistance(dropWatch.last);
    dropWatch.last = position;
    logger::debug(
        "[CarryDropWatch] target={:08X} t={:.1f} knock={} ragdoll={} life={} ai={} combat={} combatTarget={:08X} "
        "package={:08X} moving={} moved={:.1f} state={} flags={:08X} driven={}/{} synced={}/{} pos=({:.1f},{:.1f},{:.1f})",
        dropWatch.id, dropWatch.age, static_cast<int>(actorState->GetKnockState()), target->IsInRagdollState(),
        static_cast<int>(actorState->GetLifeState()), target->IsAIEnabled(), target->IsInCombat(),
        target->GetActorRuntimeData().currentCombatTarget.native_handle(), package ? package->GetFormID() : 0,
        target->IsMoving(), moved, cc ? static_cast<int>(cc->context.currentState) : -1,
        cc ? static_cast<unsigned>(cc->flags.get()) : 0, drivenRead, drivenValue, syncedRead, syncedValue, position.x,
        position.y, position.z);
}
// TDM head tracking is held off only while the lift pair or the putdown Action plays.
PairStartControl::HeadtrackingLease headtracking;
// Physics restart after a safety stop (State::bodyRestartIn).
constexpr float firstBodyBackoff = 0.3f, maxBodyBackoff = 5.0f;
constexpr std::uint32_t maxBodyRetries = 10;
CarryContinuity::HitMailbox hitMailbox;
std::atomic<std::uint64_t> transferSignal{0};
struct Entry {
    int action = -1, putdown = -1;
    RE::BSFixedString bone, targetRoot;
    RE::BSFixedString syncNames[2];
    std::uint32_t key = 0xFFFFFFFF, gamepad = 0;
    CarryTargetMode mode = CarryTargetMode::kPose;
    RE::BSFixedString part;
    float fixAt = -1.0f;
    CarryWeapon weapon = CarryWeapon::kSheathed;
};
std::vector<Entry> entries;
std::atomic<int> activeEntry{-1};
std::atomic<bool> input{false};
struct ExitObservation {
    PairedSession::Ticket ticket;
    RE::ActorHandle carrier, target;
    std::uintptr_t characters[2]{};
    bool ending = false;
};
std::mutex observationMutex;
ExitObservation observation;
RE::FormID loadedTarget = 0;
constexpr std::uint32_t saveType = 0x43525931;          // CRY1
constexpr std::uint32_t contactsSaveType = 0x43525932;  // CRY2
constexpr std::uint32_t immunitySaveType = 0x4149494D;  // AIIM: IFrame leases (runtime/ActorImmunity.h)
struct SavedContacts {
    RE::FormID actor = 0;
    std::uint32_t owned = 0;
};
SavedContacts loadedContacts;
const RE::BSFixedString& AnchorName() { return entries[state.entry].bone; }
const RE::BSFixedString& TargetRoot() { return entries[state.entry].targetRoot; }
const RE::BSFixedString& Synced() {
    static const RE::BSFixedString s("bIsSynced");
    return s;
}
bool Finite(const RE::NiPoint3& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
bool ValidTransform(const RE::NiTransform& t) {
    if (!Finite(t.translate) || !std::isfinite(t.scale) || t.scale < 0.01f || t.scale > 100) return false;
    for (const auto& row : t.rotate.entry)
        for (float v : row)
            if (!std::isfinite(v)) return false;
    return true;
}
bool SameTransform(const RE::NiTransform& a, const RE::NiTransform& b) {
    if (!ValidTransform(a) || !ValidTransform(b)) return false;
    if ((a.translate - b.translate).SqrLength() > 0.0001f || std::abs(a.scale - b.scale) > 0.00001f) return false;
    for (unsigned r = 0; r < 3; ++r)
        for (unsigned c = 0; c < 3; ++c)
            if (std::abs(a.rotate.entry[r][c] - b.rotate.entry[r][c]) > 0.00001f) return false;
    return true;
}
void ClearObservation() {
    std::lock_guard lock(observationMutex);
    observation = {};
}
void RefreshScene(RE::NiAVObject* scene) {
    RE::NiUpdateData data{};
    data.flags = RE::NiUpdateData::Flag::kDisableCollision;
    scene->UpdateDownwardPass(data, 0);
}
bool RelativeRoot(RE::NiAVObject* scene, RE::NiAVObject* bone, RE::NiTransform& result) {
    result = {};
    for (unsigned depth = 0; depth < 64 && bone; ++depth, bone = bone->parent) {
        if (bone == scene) return ValidTransform(result);
        if (!ValidTransform(bone->local)) return false;
        result = bone->local * result;
    }
    return false;
}
float Length(const RE::NiPoint3& p) { return std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z); }
CarryOwnership::Scene SceneOf(RE::Actor* actor) {
    auto* cell = actor ? actor->GetParentCell() : nullptr;
    auto* world = cell ? actor->GetWorldspace() : nullptr;
    return {cell ? cell->GetFormID() : 0, world ? world->GetFormID() : 0, cell && cell->IsInteriorCell()};
}
bool Alive(RE::Actor* actor) {
    return actor && actor->Is3DLoaded() && !actor->IsDead() && !actor->IsInRagdollState() &&
           actor->AsActorState()->GetKnockState() == RE::KNOCK_STATE_ENUM::kNormal;
}
void RevealTransferScene() {
    if (state.hiddenOwned && state.hiddenScene) state.hiddenScene->SetAppCulled(false);
    state.hiddenOwned = false;
    state.hiddenScene.reset();
}
void HideTransferScene(RE::Actor* actor) {
    auto* scene = actor && actor->Is3DLoaded() ? actor->Get3D(false) : nullptr;
    if (scene == state.hiddenScene.get()) return;
    RevealTransferScene();
    if (scene) {
        state.hiddenScene.reset(scene);
        state.hiddenOwned = !scene->GetAppCulled();
        if (state.hiddenOwned) scene->SetAppCulled(true);
    }
}
void Place(RE::Actor* actor, const RE::NiPoint3& p) {
    auto* cc = actor->GetCharController();
    if (!cc || !Finite(p)) return;
    // The Actor overload also resets controller movement state and updates the
    // scene translation. The REFR-only setter leaves those at the pickup site.
    actor->SetPosition(p, true);
    actor->Update3DPosition(true);
    actor->UpdateActor3DPosition();
}
bool RestoreContacts(RE::Actor* actor, CarryOwnership::ContactFlags& ownership) {
    if (!ownership.acquired) return true;
    auto* cc = actor && actor->Is3DLoaded() ? actor->GetCharController() : nullptr;
    if (!CarryPhysics::Available(cc)) return false;
    RE::CFilter before{}, after{};
    cc->GetCollisionFilterInfo(before);
    const auto owned = ownership.acquired;
    const bool restored =
        ownership.Restore(before.filter, [&](std::uint32_t value) { return CarryPhysics::WriteFilter(cc, value); });
    cc->GetCollisionFilterInfo(after);
    logger::info("[CarryContacts] actor={:08X} before={:08X} after={:08X} owned={:08X} restored={} pending={:08X}",
        actor->GetFormID(), before.filter, after.filter, owned, restored, ownership.acquired);
    return restored;
}
void RetryContactRecovery() {
    if (!contactRecovery.flags.acquired) return;
    auto target = contactRecovery.target.get();
    if (!target || target->IsDeleted()) {
        contactRecovery = {};
        return;
    }
    if (RestoreContacts(target.get(), contactRecovery.flags)) {
        logger::info("[CarryContacts] deferred cleanup completed actor={:08X}", target->GetFormID());
        contactRecovery = {};
    }
}
bool SharedPhysicsWorld(RE::Actor* a, RE::Actor* b) {
    auto* ac = a && a->Is3DLoaded() ? a->GetCharController() : nullptr;
    auto* bc = b && b->Is3DLoaded() ? b->GetCharController() : nullptr;
    auto* aw = ac ? ac->GetHavokWorld() : nullptr;
    auto* bw = bc ? bc->GetHavokWorld() : nullptr;
    return aw && bw && aw->GetWorld1() && aw->GetWorld1() == bw->GetWorld1();
}
bool Release(bool placeOnGround, bool restoreAI = true) {
    // Detach before restoring physics. Otherwise the next scene update would
    // move the actor back onto the shoulder.
    CarryHold::Release(false);
    CarryAttachment::End(state.attachment);
    auto actor = state.target.get();
    // A cancelled handoff may still own a temporary visual transform. Restore
    // only that exact live scene; never overwrite a ragdoll or another writer.
    if (state.holding && Alive(actor.get()) && actor->Get3D(false) == state.sampledTargetScene.get()) {
        auto* scene = state.sampledTargetScene.get();
        if (SameTransform(scene->local, state.holdApplied)) {
            scene->local = state.holdOriginal;
            RefreshScene(scene);
        }
    }
    state.holding = false;
    if (actor) {
        auto* cc = actor->Is3DLoaded() ? actor->GetCharController() : nullptr;
        if (cc && cc == state.controller.get()) {
            if (placeOnGround && Alive(actor.get()) && state.hasGround) Place(actor.get(), state.ground);
            cc->flags =
                static_cast<RE::CHARACTER_FLAGS>(state.flags.Restore(static_cast<std::uint32_t>(cc->flags.get())));
            if (cc->gravity == 0) cc->gravity = state.gravity;
            if (!actor->IsInRagdollState()) cc->SetLinearVelocityImpl({0, 0, 0, 0});
        }
        if (restoreAI && state.aiChanged && !actor->IsAIEnabled()) actor->EnableAI(true);
        logger::info("[Carry] released target={:08X} aiRestored={} controllerSame={} ground={}", actor->GetFormID(),
            restoreAI && state.aiChanged && actor->IsAIEnabled(), cc && cc == state.controller.get(),
            placeOnGround && state.hasGround);
    }
    // A detached world's controller may not accept a filter write. Its copied
    // no-collision bit still belongs to this carry when the new 3D appears.
    const bool contactsRestored = RestoreContacts(actor.get(), state.filterFlags);
    if (!contactsRestored)
        logger::info("[CarryContacts] restoration deferred owned={:08X}", state.filterFlags.acquired);
    state.controller.reset();
    if (restoreAI) state.aiChanged = false;
    state.flags = {};
    return contactsRestored;
}
void Stop(const char* reason, bool ground = true) {
    logger::info("[Carry] stop reason={} phase={} age={:.2f} moved={:.2f}", reason, static_cast<int>(state.phase),
        state.age, state.distance);
    CarryWeaponCover::End(reason);
    if (state.phase == Phase::kLifting) PairedSession::Cancel(state.ticket);
    headtracking.Release();
    {
        // Bodies go back to the pose (or stay with the engine) before AI and the controller return.
        auto target = state.target.get();
        CarryBody::End(target && (target->IsDead() || target->IsInRagdollState()));
    }
    CarryPoseLock::Release(reason);
    activeEntry = -1;
    input = false;
    hitMailbox.Reset();
    transferSignal = 0;
    RevealTransferScene();
    ClearObservation();
    state.startControl.Release();
    Release(ground);
    PrecisionLink::Release(state.target);
    if (state.filterFlags.acquired) {
        auto target = state.target.get();
        contactRecovery = {state.target, RE::NiPointer<RE::TESObjectREFR>(target.get()), state.filterFlags};
    }
    state = {};
}
// A weapon, a hit or a knocked-down carrier ends the carry without ragdoll pair effects: a lift pair
// is cancelled (Stop does that), and a target that is already held is dropped. The putdown's
// CarryRelease moment drops the target through this same path.
void CancelCarry(const char* reason) {
    const bool held = state.phase == Phase::kCarrying || state.phase == Phase::kPuttingDown;
    const auto target = state.target;
    const auto id = state.targetId;
    // Keep the swinging bodies' poses and velocities for the engine's ragdoll.
    if (held) CarryBody::BeginRelease();
    Stop(reason, false);
    if (!held) return;
    drop = {target, id};
    drop.handoff.Start();
    logger::info("[CarryDrop] queued reason={} target={:08X}", reason, id);
}
void AdvanceDrop(float delta) {
    if (!drop.handoff.active) return;
    auto target = drop.target.get();
    const bool waitFor3D = target && drop.fromLoad && !target->Is3DLoaded() && std::isfinite(delta) &&
                           (drop.waited += std::max(delta, 0.0f)) < 15.0f;
    if (waitFor3D) return;
    if (!target || target->IsDeleted() || target->IsDead() || !target->Is3DLoaded()) {
        logger::info("[CarryDrop] abandoned target={:08X}: unavailable", drop.id);
        drop = {};
        return;
    }
    using CarryDrop::HandoffResult;
    const auto result = drop.handoff.Advance(
        delta,
        [&] {
            if (contactRecovery.flags.acquired && contactRecovery.target == drop.target) return false;
            auto* cc = target->GetCharController();
            // Release() restored our flags, gravity and AI. A loaded target has no flags of ours in
            // memory; the engine may still hold it without simulation until the knockdown.
            return cc && (drop.fromLoad || !cc->flags.all(RE::CHARACTER_FLAGS::kNoSim));
        },
        [&] {
            if (target->IsInRagdollState()) return true;
            auto* process = target->GetActorRuntimeData().currentProcess;
            if (!process || target->IsOnMount()) return false;
            // The minimal knockdown of the 0.12.132 ragdoll release; the engine then gets it up.
            auto origin = target->GetPosition();
            origin.z -= 30;
            process->KnockExplosion(target.get(), origin, 1.0f);
            logger::info("[CarryDrop] knockdown requested target={:08X}", drop.id);
            return true;
        },
        [&] {
            if (CarryBody::Current() == CarryBody::State::kReleasing) {
                const auto physical = CarryBody::AdvanceRelease();
                if (physical == CarryBody::Release::kWaiting) return HandoffResult::kWaiting;
                if (physical == CarryBody::Release::kDone) return HandoffResult::kComplete;
            }
            return target->IsInRagdollState() ? HandoffResult::kComplete : HandoffResult::kWaiting;
        });
    if (result == HandoffResult::kWaiting) return;
    logger::info("[CarryDrop] {} target={:08X} wait={:.3f} requested={}",
        result == HandoffResult::kComplete ? "ragdoll" : "failed", drop.id, drop.handoff.elapsed,
        drop.handoff.requested);
    StartDropWatch(target.get());
    drop = {};
}
bool Attach(RE::Actor* carrier, RE::Actor* target) {
    auto* cc = target->GetCharController();
    auto* root = carrier->Get3D(false);
    auto* bone = root ? root->GetObjectByName(AnchorName()) : nullptr;
    auto* anchor = bone ? bone->AsNode() : nullptr;
    auto* targetScene = target->Get3D(false);
    auto* targetRoot = targetScene ? targetScene->GetObjectByName(TargetRoot()) : nullptr;
    bool synced = true;
    if (!CarryPhysics::Available(cc) || !anchor || !targetRoot || !target->GetGraphVariableBool(Synced(), synced) ||
        synced || !std::isfinite(cc->gravity))
        return false;
    RE::NiTransform rootInScene;
    if (!state.endPose.valid || !state.endPose.frozen || root != state.sampledCarrierScene.get() ||
        targetScene != state.sampledTargetScene.get() || anchor != state.sampledAnchor.get() ||
        !ValidTransform(bone->world) || !ValidTransform(targetScene->world) ||
        !RelativeRoot(targetScene, targetRoot, rootInScene))
        return false;
    // Preserve the authored root-to-anchor position AND rotation. Recompute
    // scene placement from the current root local after the pair has retired.
    const auto desiredRoot = state.endPose.Root(bone->world, targetScene->world.scale * rootInScene.scale);
    const auto desiredScene = CarryPlacement::Solve(desiredRoot, rootInScene, RE::NiPoint3{}).scene;
    state.syncOffset = CarryPlacement::NativeOffset(bone->world, desiredScene);
    if (!ValidTransform(state.syncOffset) || Length(state.syncOffset.translate) > 180 ||
        !CarryAttachment::Begin(state.attachment, carrier, target, anchor, state.syncOffset))
        return false;
    state.holding = false;
    constexpr auto wanted = static_cast<std::uint32_t>(RE::CHARACTER_FLAGS::kNoSim) |
                            static_cast<std::uint32_t>(RE::CHARACTER_FLAGS::kNoCharacterCollisions);
    state.controller.reset(cc);
    state.gravity = cc->gravity;
    state.aiChanged = state.aiChanged || target->IsAIEnabled();
    if (target->IsAIEnabled()) target->EnableAI(false);
    cc->flags =
        static_cast<RE::CHARACTER_FLAGS>(state.flags.Acquire(static_cast<std::uint32_t>(cc->flags.get()), wanted));
    cc->gravity = 0;
    cc->SetLinearVelocityImpl({0, 0, 0, 0});
    RE::CFilter original{};
    cc->GetCollisionFilterInfo(original);
    const auto disabled = state.filterFlags.Acquire(original.filter, RE::CFilter::Flags::kNoCollision);
    if (!CarryPhysics::WriteFilter(cc, disabled)) return false;
    {
        std::lock_guard lock(observationMutex);
        for (unsigned i = 0; i < 2; ++i) state.characters[i] = observation.characters[i];
    }
    state.phase = Phase::kCarrying;
    state.age = state.trace = state.distance = 0;
    state.targetId = target->GetFormID();
    state.targetLifetime.reset(target);
    if (!hitMailbox.Watch(carrier->GetFormID())) {
        hitMailbox.Arm(carrier->GetFormID(), target->GetFormID());
    }
    state.lastCarrier = carrier->GetPosition();
    state.hasGround = CarryGround::InFront(carrier, state.ground);
    state.startControl.Release();
    logger::info("[CarryPhysics] filter target={:08X} before={:08X} carried={:08X} owned={:08X}", target->GetFormID(),
        original.filter, disabled, state.filterFlags.acquired);
    logger::info(
        "[Carry] attached nativeSync target={:08X} anchor={} root={} originalGravity={} ownedFlags={:X} aiPaused={} rootLocal=({:.3f},{:.3f},{:.3f}) poseLocked={}",
        target->GetFormID(), AnchorName().c_str(), TargetRoot().c_str(), state.gravity, state.flags.acquired,
        state.aiChanged, rootInScene.translate.x, rootInScene.translate.y, rootInScene.translate.z,
        CarryPoseLock::Captured());
    CarryHold::Hold();
    // The carried body now rides the grip, so the carrier's head and spine may track again.
    headtracking.Release();
    // Not yet: the bodies start from the scene's pose, and the native sync re-places the scene on its
    // first pulse. With the lift's travel kept in the locked pose the scene origin sits far below the
    // grip, so even a slight re-placement split the anchor from its simulated neighbours (0.12.188:
    // 3.5 units at the first step). The update starts the bodies after that pulse has been drawn.
    state.bodyPending = entries[state.entry].mode == CarryTargetMode::kPhysics;
    state.bodyRestartIn = -1;
    state.bodyBackoff = state.bodyAge = 0;
    state.bodyRestarts = state.bodyFailedStarts = 0;
    return true;
}
bool CheckInterruption(RE::Actor* carrier, RE::Actor* target) {
    for (unsigned role = 0; role < 2; ++role) {
        auto* actor = role ? target : carrier;
        if (!actor || actor->IsDeleted() || actor->IsDisabled()) {
            Stop("participant-removed", false);
            return true;
        }
        if (actor->IsDead()) {
            Stop(role ? "target-dead" : "carrier-dead", false);
            return true;
        }
        if (actor->Is3DLoaded() &&
            (actor->IsInRagdollState() || actor->AsActorState()->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal)) {
            // A ragdolled target is already physical; a knocked-down carrier drops what it holds.
            if (role)
                Stop("target-ragdoll", false);
            else
                CancelCarry("carrier-ragdoll");
            return true;
        }
        if (hitMailbox.Consume(role)) {
            logger::info("[CarryHit] role={} actor={:08X}", role, actor->GetFormID());
            CancelCarry(role ? "target-hit" : "carrier-hit");
            return true;
        }
    }
    // A drawn weapon or spell drops the target (an attack, while weapon="sheathe" has not put it away).
    if (const char* reason = CarryWeaponCover::Rule(carrier)) {
        CancelCarry(reason);
        return true;
    }
    return false;
}
void Suspend(const char* reason) {
    headtracking.Release();
    CarryBody::End(false);
    if (state.phase == Phase::kSuspended) return;
    // A putdown cut by a load or cell change resumes as a carry in the new cell.
    if (state.phase != Phase::kCarrying && state.phase != Phase::kPuttingDown) return;
    if (!state.endPose.valid || !state.endPose.frozen) {
        Stop("transfer-pose-unavailable", false);
        return;
    }
    logger::info("[CarryTransfer] suspend reason={} target={:08X} age={:.2f}", reason, state.targetId, state.age);
    state.startControl.Release();
    Release(false, false);  // Keep the original AI restoration obligation across 3D replacement.
    ClearObservation();
    state.characters[0] = state.characters[1] = 0;
    state.sampledCarrierScene.reset();
    state.sampledTargetScene.reset();
    state.sampledAnchor.reset();
    state.hasGround = false;
    state.transfer = {};
    state.syncFailureTime = 0;
    state.phase = Phase::kSuspended;
    input = false;
    auto target = state.target.get();
    HideTransferScene(target.get());
}
bool CurrentCharacters(RE::Actor* carrier, RE::Actor* target, std::uintptr_t (&out)[2]) {
    for (unsigned role = 0; role < 2; ++role) {
        auto* actor = role ? target : carrier;
        RE::BSAnimationGraphManagerPtr manager;
        if (!actor->GetAnimationGraphManager(manager) || !manager) return false;
        RE::BSSpinLockGuard lock(manager->GetRuntimeData().updateLock);
        const auto rig =
            role ? Actions::Spec(entries[state.entry].action).paired.targetRig : PairedRig::Kind::kCharacter;
        out[role] = 0;
        for (const auto& graph : manager->graphs)
            if (graph) {
                const auto& ch = graph->characterInstance;
                const auto* strings = ch.setup && ch.setup->data ? ch.setup->data->stringData.get() : nullptr;
                if (strings && strings->name.c_str() && PairedRig::CharacterName(strings->name.c_str()) == rig) {
                    if (out[role]) return false;
                    out[role] = reinterpret_cast<std::uintptr_t>(&ch);
                }
            }
        if (!out[role]) return false;
    }
    return true;
}
bool HoldTransferPhysics(RE::Actor* target) {
    auto* cc = target->Is3DLoaded() ? target->GetCharController() : nullptr;
    if (!CarryPhysics::Available(cc)) return false;
    if (state.controller.get() == cc) {
        RE::CFilter filter{};
        cc->GetCollisionFilterInfo(filter);
        return filter.QNoCollision() && cc->flags.all(RE::CHARACTER_FLAGS::kNoSim) && cc->gravity == 0;
    }
    Release(false, false);
    if (!std::isfinite(cc->gravity)) return false;
    state.controller.reset(cc);
    state.gravity = cc->gravity;
    constexpr auto flags = static_cast<std::uint32_t>(RE::CHARACTER_FLAGS::kNoSim) |
                           static_cast<std::uint32_t>(RE::CHARACTER_FLAGS::kNoCharacterCollisions);
    state.flags.Acquire(static_cast<std::uint32_t>(cc->flags.get()), flags);
    cc->flags = static_cast<RE::CHARACTER_FLAGS>(static_cast<std::uint32_t>(cc->flags.get()) | flags);
    cc->gravity = 0;
    cc->SetLinearVelocityImpl({0, 0, 0, 0});
    RE::CFilter filter{};
    cc->GetCollisionFilterInfo(filter);
    return CarryPhysics::WriteFilter(cc, state.filterFlags.Acquire(filter.filter, RE::CFilter::Flags::kNoCollision));
}
void ContinueTransfer(RE::Actor* carrier, RE::Actor* target, float delta) {
    auto* ui = RE::UI::GetSingleton();
    auto* cell = carrier->GetParentCell();
    const bool paused = !ui || ui->GameIsPaused() || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
    const bool ready = carrier->Is3DLoaded() && cell && cell->IsAttached() && carrier->GetCharController();
    if (cell) state.transfer.Destination(cell->GetFormID());
    if (state.transfer.Tick(delta, paused, ready)) {
        Stop("transfer-rebind-failed", false);
        return;
    }
    if (paused || !ready) return;
    HideTransferScene(target);
    if (!CarryOwnership::SameScene(SceneOf(carrier), SceneOf(target))) {
        if (state.transfer.CanMove()) {
            state.transfer.Moved();
            logger::info("[CarryTransfer] move target={:08X} destination={:08X} attempt={}", target->GetFormID(),
                cell->GetFormID(), state.transfer.attempts);
            // Public engine API transfers cell/world ownership as well as position.
            // Do not call Load3D or write a foreign world's controller directly.
            Release(false, false);
            target->MoveTo(carrier);
            HideTransferScene(target);
            if (CarryOwnership::SameScene(SceneOf(carrier), SceneOf(target)) && SharedPhysicsWorld(carrier, target))
                HoldTransferPhysics(target);
        }
        state.transfer.Ready(false);
        return;
    }
    auto* targetScene = target->Is3DLoaded() ? target->Get3D(false) : nullptr;
    auto* carrierScene = carrier->Get3D(false);
    auto* bone = carrierScene ? carrierScene->GetObjectByName(AnchorName()) : nullptr;
    auto* targetRoot = targetScene ? targetScene->GetObjectByName(TargetRoot()) : nullptr;
    auto* controller = target->Is3DLoaded() ? target->GetCharController() : nullptr;
    bool synced = true;
    std::uintptr_t characters[2]{};
    const bool components = targetScene && bone && bone->AsNode() && targetRoot &&
                            SharedPhysicsWorld(carrier, target) && HoldTransferPhysics(target) &&
                            target->GetGraphVariableBool(Synced(), synced) && !synced &&
                            CurrentCharacters(carrier, target, characters);
    if (!state.transfer.Ready(components)) return;
    state.sampledCarrierScene.reset(carrierScene);
    state.sampledTargetScene.reset(targetScene);
    state.sampledAnchor.reset(bone->AsNode());
    const auto age = state.age, distance = state.distance;
    const auto attempts = state.transfer.attempts;
    const auto waited = state.transfer.readyAge;
    Release(false, false);  // Reacquire the new physics lease atomically with attachment.
    if (!Attach(carrier, target)) {
        Stop("transfer-attach-failed", false);
        return;
    }
    state.characters[0] = characters[0];
    state.characters[1] = characters[1];
    CarryPoseLock::Rebind(characters[1]);
    state.age = age;
    state.distance = distance;
    state.transfer = {};
    if (!CarryAttachment::Pulse(state.attachment, carrier, target, bone->AsNode()) ||
        !CarryPhysics::Follow(controller, target->GetPosition())) {
        Stop("transfer-sync-failed", false);
        return;
    }
    // Reveal only after the new reference, skin and capsule share the held pose.
    RevealTransferScene();
    transferSignal = 0;
    state.lastCarrierCell = cell->GetFormID();
    state.lastTargetCell = target->GetParentCell()->GetFormID();
    logger::info(
        "[CarryTransfer] resumed target={:08X} cell={:08X} attempts={} activeWait={:.3f}; authored relative pose preserved",
        target->GetFormID(), cell->GetFormID(), attempts, waited);
}
bool HoldEndPose(RE::Actor* carrier, RE::Actor* target) {
    auto* carrierScene = carrier->Get3D(false);
    auto* scene = target->Get3D(false);
    auto* anchor = carrierScene ? carrierScene->GetObjectByName(AnchorName()) : nullptr;
    auto* root = scene ? scene->GetObjectByName(TargetRoot()) : nullptr;
    RE::NiTransform local;
    if (!state.endPose.valid || !state.endPose.frozen || !scene || !root || !anchor ||
        carrierScene != state.sampledCarrierScene.get() || scene != state.sampledTargetScene.get() ||
        anchor != state.sampledAnchor.get() || !ValidTransform(anchor->world) || !ValidTransform(scene->world) ||
        !RelativeRoot(scene, root, local))
        return false;
    const auto desiredRoot = state.endPose.Root(anchor->world, scene->world.scale * local.scale);
    const auto desiredScene = CarryPlacement::Solve(desiredRoot, local, RE::NiPoint3{}).scene;
    if (!ValidTransform(desiredScene) || Length(desiredScene.translate - carrier->GetPosition()) > 220 ||
        Length(anchor->world.translate - carrier->GetPosition()) > 180)
        return false;
    if (!state.holding || !SameTransform(scene->local, state.holdApplied)) state.holdOriginal = scene->local;
    RE::NiUpdateData data{};
    data.flags = RE::NiUpdateData::Flag::kDisableCollision;
    CarryPlacement::ApplyScene(*scene, desiredScene, data);
    state.holdApplied = scene->local;
    state.holding = true;
    return true;
}
// The fixed target's root is written into its animation output, so it must be the animation
// skeleton's root (graph bone 0, the pose track's bone 0) and the carry's target root.
bool ResolveFollowRoot(RE::Actor* target) {
    RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
    if (!target->GetAnimationGraphManager(manager) || !manager) return false;
    RE::BSSpinLockGuard lock(manager->GetRuntimeData().updateLock);
    const auto graph = manager->GetRuntimeData().activeGraph;
    if (graph >= manager->graphs.size() || !manager->graphs[graph]) return false;
    const auto& bones = manager->graphs[graph]->boneNodes;
    if (bones.empty() || !bones[0].node) return false;
    const auto entry = static_cast<std::int32_t>(bones[0].unk08);
    const auto name = GraphBoneSlot::Name(bones[0].node, entry);
    if (!name.c_str() || _stricmp(name.c_str(), TargetRoot().c_str()) != 0) {
        logger::warn("[CarryFix] animation root '{}' is not targetRoot '{}'; pose locked without following",
            name.c_str() ? name.c_str() : "", TargetRoot().c_str());
        return false;
    }
    const auto slot = GraphBoneSlot::Access(bones[0].node, entry);
    if (!slot.world || !slot.parent) return false;
    state.rootOwner.reset(bones[0].node);
    state.rootEntry = entry;
    return true;
}
// Main thread, every lift frame after the fix moment (or the lift's end): the root where the frozen grip puts
// it. The hand is the one drawn last frame, so the root trails it by one frame of hand travel.
void FollowRoot(RE::Actor* carrier, RE::Actor* target) {
    auto* carrierScene = carrier->Get3D(false);
    auto* scene = target->Get3D(false);
    auto* anchor = carrierScene ? carrierScene->GetObjectByName(AnchorName()) : nullptr;
    if (!anchor || carrierScene != state.sampledCarrierScene.get() || scene != state.sampledTargetScene.get() ||
        anchor != state.sampledAnchor.get()) {
        state.follows = false;
        logger::warn("[CarryFix] carrier bone or target 3D changed; following stopped");
        return;
    }
    const auto slot = GraphBoneSlot::Access(state.rootOwner.get(), state.rootEntry);
    if (!slot.world || !slot.parent || !ValidTransform(anchor->world) || !ValidTransform(*slot.world) ||
        !ValidTransform(*slot.parent))
        return;
    const auto desired = state.endPose.Root(anchor->world, slot.world->scale);
    const auto local = slot.parent->Invert() * desired;
    if (!ValidTransform(desired) || !ValidTransform(local) || Length(local.translate) > 400) return;
    const RE::NiQuaternion q(local.rotate);
    CarryPoseLock::Follow({{local.translate.x, local.translate.y, local.translate.z}, {q.x, q.y, q.z, q.w}});
}
class TransferEvents final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>,
                             public RE::BSTEventSink<RE::TESCellAttachDetachEvent> {
    RE::BSEventNotifyControl ProcessEvent(
        const RE::MenuOpenCloseEvent* e, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
        static const RE::BSFixedString loading("Loading Menu");
        if (e && e->opening && e->menuName == loading) transferSignal = hitMailbox.watched[0].load();
        return RE::BSEventNotifyControl::kContinue;
    }
    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESCellAttachDetachEvent* e, RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override {
        if (e && !e->attached && e->reference)
            if (const auto token = hitMailbox.Watch(e->reference->GetFormID())) transferSignal = token;
        return RE::BSEventNotifyControl::kContinue;
    }
};
bool PutdownKey(const Entry& config, const RE::ButtonEvent& event) {
    const auto device = event.device.get();
    const auto code = event.GetIDCode();
    return (device == RE::INPUT_DEVICE::kKeyboard && code == config.key) ||
           (device == RE::INPUT_DEVICE::kMouse && code + 256 == config.key) ||
           (device == RE::INPUT_DEVICE::kGamepad && config.gamepad && code == config.gamepad);
}
void Saved(SKSE::SerializationInterface* s) {
    // AI enablement is saved by the game. Persist only our recovery obligation,
    // never a pointer or an active carry session. Loading returns the target to AI.
    if (state.aiChanged)
        if (auto target = state.target.get()) {
            const auto id = target->GetFormID();
            if (!s->WriteRecord(saveType, 1, id)) logger::error("[Carry] failed to save AI recovery record");
        }
    // Only our no-collision bit is recoverable. Never serialize whole filters:
    // collision groups belong to the destination world's current controller.
    const auto handle = state.filterFlags.acquired ? state.target : contactRecovery.target;
    const auto owned = state.filterFlags.acquired ? state.filterFlags.acquired : contactRecovery.flags.acquired;
    if (owned)
        if (auto target = handle.get()) {
            const SavedContacts record{target->GetFormID(), owned};
            if (!s->WriteRecord(contactsSaveType, 1, record))
                logger::error("[CarryContacts] failed to save collision recovery record");
        }
    // The plugin has one co-save (one unique id): event functions' records go in it too.
    EventEffects::Save(s);
    // IFrame leases held now (runtime/ActorImmunity.h): the game saves their actors as ghosts.
    // Version 2: actors and their state before the lease. Version 1 (0.12.229): bases and their bits.
    if (const auto leases = ActorImmunity::Snapshot(); !leases.empty()) {
        if (s->OpenRecord(immunitySaveType, 2)) {
            s->WriteRecordData(static_cast<std::uint32_t>(leases.size()));
            for (const auto& lease : leases) {
                const std::uint8_t wasGhost = lease.wasGhost ? 1 : 0;
                s->WriteRecordData(lease.actor);
                s->WriteRecordData(wasGhost);
            }
        } else
            logger::error("[Immunity] failed to save the lease record");
    }
}
void Loaded(SKSE::SerializationInterface* s) {
    loadedTarget = 0;
    loadedContacts = {};
    std::uint32_t type = 0, version = 0, size = 0;
    while (s->GetNextRecordInfo(type, version, size))
        if (type == saveType && version == 1 && size == sizeof(RE::FormID)) {
            RE::FormID old = 0, resolved = 0;
            if (s->ReadRecordData(old) == sizeof(old) && s->ResolveFormID(old, resolved)) loadedTarget = resolved;
        } else if (type == contactsSaveType && version == 1 && size == sizeof(SavedContacts)) {
            SavedContacts record{};
            RE::FormID resolved = 0;
            if (s->ReadRecordData(record) == sizeof(record) && s->ResolveFormID(record.actor, resolved))
                loadedContacts = {resolved, record.owned & RE::CFilter::Flags::kNoCollision};
        } else if (type == immunitySaveType && (version == 1 || version == 2)) {
            std::uint32_t count = 0;
            std::vector<ActorImmunity::SavedLease> leases;
            std::vector<ActorImmunity::SavedBase> bases;
            if (s->ReadRecordData(count) == sizeof(count))
                for (std::uint32_t i = 0; i < count && i < 4096; ++i) {
                    RE::FormID id = 0, resolved = 0;
                    std::uint8_t bits = 0;
                    if (s->ReadRecordData(id) != sizeof(id) || s->ReadRecordData(bits) != sizeof(bits)) break;
                    if (!s->ResolveFormID(id, resolved)) continue;
                    if (version == 2)
                        leases.push_back({resolved, (bits & 1) != 0});
                    else
                        bases.push_back({resolved, (bits & 1) != 0, (bits & 2) != 0});
                }
            ActorImmunity::SetLoaded(std::move(leases));
            ActorImmunity::SetLoadedBases(std::move(bases));
        } else
            EventEffects::LoadRecord(s, type, version, size);
}
}
void InstallSerialization() {
    if (auto* s = SKSE::GetSerializationInterface()) {
        s->SetUniqueID(0x41494E4A);
        s->SetSaveCallback(Saved);
        s->SetLoadCallback(Loaded);
    }
}
void Cancel(const char* reason) {
    if (state.phase == Phase::kIdle) return;
    CancelCarry(reason);
}
bool Enabled() { return !entries.empty(); }
bool ClaimInput(const RE::ButtonEvent& event, bool allow) {
    const int active = activeEntry.load();
    if (!allow || active < 0 || active >= static_cast<int>(entries.size()) || !PutdownKey(entries[active], event))
        return false;
    // While a carry lasts its key is the carry's alone (press, hold and release): the game never
    // acts on it too, so E puts the target down without also activating what the carrier faces.
    if (event.IsDown()) input = true;
    return true;
}
void Register() {
    for (int i = 0; i < Actions::Count(); ++i)
        if (Actions::Spec(i).carry.enabled) {
            const auto& spec = Actions::Spec(i);
            const auto& carry = spec.carry;
            const int putdown = Actions::IndexOf(carry.putdownAction);
            if (!spec.paired.enabled || spec.clips.size() != 2 || putdown < 0 || putdown == i ||
                Actions::Spec(putdown).paired.enabled || Actions::Spec(putdown).carry.enabled ||
                !Actions::Spec(putdown).carryRelease.enabled || !Actions::Spec(putdown).flow.nodes.empty() ||
                !ValidCarryBone(carry.bone) || !ValidCarryBone(carry.targetRoot) ||
                (!std::isfinite(spec.enterBlend) || spec.enterBlend < 0 || spec.enterBlend > 5) ||
                (carry.key > 265 && !carry.gamepad)) {
                logger::error("[Carry] '{}' invalid carry configuration; carry start disabled", spec.id);
                continue;
            }
            entries.push_back(
                {i, putdown, RE::BSFixedString(carry.bone.c_str()), RE::BSFixedString(carry.targetRoot.c_str()),
                    {RE::BSFixedString(spec.clips[1].enterEvent.c_str()),
                        RE::BSFixedString(spec.clips[0].enterEvent.c_str())},
                    carry.key, carry.gamepad, carry.mode, RE::BSFixedString(carry.part.c_str()), carry.fixAt,
                    carry.weapon});
            logger::info(
                "[Carry] configured action={} bone='{}' targetRoot='{}' putdown={} key={} pad={} target={} part='{}' fixAt={:.3f} weapon={}",
                spec.id, carry.bone, carry.targetRoot, carry.putdownAction, carry.key, carry.gamepad,
                carry.mode == CarryTargetMode::kPhysics ? "physics" : "pose", carry.part, carry.fixAt,
                CarryWeaponName(carry.weapon));
        }
    if (entries.empty()) return;
    static TransferEvents transferEvents;
    if (auto* ui = RE::UI::GetSingleton()) ui->AddEventSink<RE::MenuOpenCloseEvent>(&transferEvents);
    if (auto* events = RE::ScriptEventSourceHolder::GetSingleton())
        events->AddEventSink<RE::TESCellAttachDetachEvent>(&transferEvents);
}
bool CanBegin(int action, RE::Actor* carrier, RE::Actor* target) {
    if (state.phase != Phase::kIdle || contactRecovery.flags.acquired || drop.handoff.active || !Alive(carrier) ||
        !Alive(target) || carrier != RE::PlayerCharacter::GetSingleton() || carrier == target || carrier->IsInMidair())
        return false;
    for (const auto& entry : entries)
        if (entry.action == action) {
            if (entry.weapon == CarryWeapon::kSheathed && CarryWeaponCover::WeaponOut(carrier)) {
                logger::info("[Carry] start rejected: weapon or spell out");
                return false;
            }
            auto* scene = carrier->Get3D(false);
            auto* targetScene = target->Get3D(false);
            auto* anchor = scene ? scene->GetObjectByName(entry.bone) : nullptr;
            auto* root = targetScene ? targetScene->GetObjectByName(entry.targetRoot) : nullptr;
            if (anchor && anchor->AsNode() && root) return true;
            logger::warn("[Carry] start rejected: bone='{}' found={} targetRoot='{}' found={}", entry.bone.c_str(),
                anchor && anchor->AsNode(), entry.targetRoot.c_str(), root != nullptr);
            return false;
        }
    return false;
}
void PairStarted(int action, const PairedSession::Status& pair) {
    if (state.phase != Phase::kIdle || pair.phase == PairedSession::Phase::kIdle) return;
    for (int i = 0; i < static_cast<int>(entries.size()); ++i)
        if (entries[i].action == action) {
            state = {};
            CarryPoseLock::Release("lift-start");
            state.entry = i;
            state.phase = Phase::kLifting;
            state.carrier = pair.initiator.actor;
            state.target = pair.target.actor;
            state.ticket = pair.ticket;
            {
                std::lock_guard lock(observationMutex);
                observation = {pair.ticket, pair.initiator.actor, pair.target.actor};
            }
            // Submit has reserved the pair but native mark placement has not run.
            // Protect the lift's native start.
            auto carrier = state.carrier.get(), target = state.target.get();
            if (!state.startControl.Acquire(carrier.get(), target.get())) {
                Stop("start-control-unavailable", false);
                return;
            }
            headtracking.Acquire();
            if (Alive(carrier.get()) && Alive(target.get())) {
                state.sampledCarrierScene.reset(carrier->Get3D(false));
                state.sampledTargetScene.reset(target->Get3D(false));
                // Hits and a drawn weapon end the carry from the lift on, not only once it is held.
                hitMailbox.Arm(carrier->GetFormID(), target->GetFormID());
            }
            input = false;
            activeEntry = i;
            CarryWeaponCover::Begin(entries[i].weapon, state.carrier, state.target);
            // Precision's hit reaction would still own the target's bodies when the carry attaches.
            PrecisionLink::Hold(state.target);
            CarryHold::Begin(state.carrier, Actions::Spec(action).carry.hold);
            logger::info("[Carry] configured lift requested action={} request={}", Actions::Spec(action).id,
                pair.ticket.request);
            return;
        }
}
StartGate PrepareStart(PairedSession::Ticket ticket) {
    if (state.ticket != ticket || state.phase != Phase::kLifting) return StartGate::kReady;
    switch (state.startControl.Ready()) {
    case PairStartControl::Readiness::kReady: return StartGate::kReady;
    case PairStartControl::Readiness::kFailed: return StartGate::kFailed;
    default: return StartGate::kWaiting;
    }
}
bool ReadNativeStartBridge(RE::ActorHandle support, RE::ActorHandle leader,
    const std::array<RE::BSFixedString, 2>& generators, NativeStartBridge& output) {
    if (state.startHanded || state.phase != Phase::kLifting || state.entry < 0 ||
        state.entry >= static_cast<int>(entries.size()) || !state.startControl.active ||
        !state.startControl.cameraToken || state.startControl.Ready() != PairStartControl::Readiness::kReady)
        return false;
    const auto pair = PairedSession::ReadStatus();
    if (pair.ticket != state.ticket || pair.phase != PairedSession::Phase::kAwaitingBoth ||
        pair.initiator.actor != state.carrier || pair.target.actor != state.target)
        return false;
    if (!((support == state.carrier && leader == state.target) || (support == state.target && leader == state.carrier)))
        return false;
    NativeStartBridge result;
    result.ticket = state.ticket;
    result.camera = state.startControl.cameraToken;
    const std::array<RE::ActorHandle, 2> actors{support, leader};
    for (unsigned i = 0; i < 2; ++i) {
        const unsigned role = actors[i] == state.carrier ? 0 : 1;
        if (generators[i] != entries[state.entry].syncNames[role]) return false;
        auto actor = actors[i].get();
        auto* scene = role ? state.sampledTargetScene.get() : state.sampledCarrierScene.get();
        if (!Alive(actor.get()) || actor->Get3D(false) != scene) return false;
        auto& out = result.roles[i];
        out.actor = actors[i];
        out.scene.reset(scene);
        out.generatorName = generators[i];
    }
    output = std::move(result);
    return true;
}
bool CommitNativeStartBridge(const NativeStartBridge& bridge) {
    NativeStartBridge current;
    if (!ReadNativeStartBridge(bridge.roles[0].actor, bridge.roles[1].actor,
            {bridge.roles[0].generatorName, bridge.roles[1].generatorName}, current) ||
        current.ticket != bridge.ticket || current.camera != bridge.camera)
        return false;
    for (unsigned i = 0; i < 2; ++i)
        if (current.roles[i].scene != bridge.roles[i].scene) return false;
    if (!PairStartTransfer::ClaimCamera(state.startControl.cameraToken, bridge.camera)) return false;
    state.startHanded = true;
    logger::info("[CarryStartBridge] #{} transferred to common native start camera={}; carry steering retained",
        bridge.ticket.request, bridge.camera);
    return true;
}
void AbandonNativeStartBridge(PairedSession::Ticket ticket) {
    if (state.ticket != ticket || state.startHanded || state.phase != Phase::kLifting) return;
    // Only the unconsumed bridge is discarded. Carry/session cleanup still owns
    // the steering and physical leases; never release a different native owner.
    state.startHanded = true;
    logger::warn(
        "[CarryStartBridge] #{} common native start unavailable; released held pose to engine", ticket.request);
}
void ObserveClip(PairedSession::Ticket ticket, RE::ActorHandle actor, const RE::hkbContext& context,
    const RE::BSSynchronizedClipGenerator& generator, bool exiting) {
    const int entry = activeEntry.load();
    if (entry < 0 || entry >= static_cast<int>(entries.size()) || !context.character) return;
    std::lock_guard lock(observationMutex);
    if (!(observation.ticket == ticket) || !ticket.request) return;
    const int role = actor == observation.carrier ? 0 : actor == observation.target ? 1 : -1;
    if (role < 0) return;
    const auto* name = generator.name.c_str();
    const auto& expected = entries[entry].syncNames[role];
    if (!name || std::strcmp(name, expected.c_str()) != 0 || generator.leadCharacter != (role == 1)) return;
    observation.characters[role] = reinterpret_cast<std::uintptr_t>(context.character);
    observation.ending |= exiting;
    // The target's own lift clip output is what the carry keeps from the fix moment or the lift's end.
    if (role == 1 && !exiting && generator.clipGenerator)
        CarryPoseLock::Watch(reinterpret_cast<std::uintptr_t>(context.character),
            reinterpret_cast<std::uintptr_t>(generator.clipGenerator), entries[entry].fixAt);
    if (exiting) CarryPoseLock::CaptureNow();
    // The carrier's own lift clip, sampled on the thread evaluating it.
    if (role == 0 && !exiting && generator.clipGenerator) CarryHold::SampleClip(*generator.clipGenerator);
}
void ObserveExitBlend(const RE::hkbContext& context) {
    if (activeEntry.load() < 0 || !context.character) return;
    std::lock_guard lock(observationMutex);
    const auto character = reinterpret_cast<std::uintptr_t>(context.character);
    if (observation.ticket.request &&
        (observation.characters[0] == character || observation.characters[1] == character)) {
        observation.ending = true;
        // Before the first blend step: the target never blends toward its standing pose.
        CarryPoseLock::CaptureNow();
    }
}
void ObserveEndEvent(PairedSession::Ticket ticket, RE::ActorHandle actor) {
    if (activeEntry.load() < 0) return;
    std::lock_guard lock(observationMutex);
    if (ticket.request && observation.ticket == ticket &&
        (actor == observation.carrier || actor == observation.target)) {
        observation.ending = true;
        CarryPoseLock::CaptureNow();
    }
}
void ReleaseMovementControl(PairedSession::Ticket ticket, RE::ActorHandle actor) {
    if (ticket != state.ticket || actor != state.carrier || state.phase != Phase::kLifting) return;
    state.startControl.ReleaseForMovement(actor);
}
void Sample() {
    // Copy the lift's last playing pose; frozen at the fix moment or once the lift starts to end.
    if (state.phase != Phase::kLifting || state.liftEnded || (state.endPose.frozen && !state.endPose.valid)) return;
    const auto pair = PairedSession::ReadStatus();
    if (!(state.ticket == pair.ticket)) return;
    bool ending = false;
    {
        std::lock_guard lock(observationMutex);
        ending = observation.ticket == state.ticket && observation.ending;
    }
    ending |= pair.initiator.endEventSeen || pair.target.endEventSeen ||
              pair.phase == PairedSession::Phase::kStopping || pair.phase == PairedSession::Phase::kIdle;
    if (ending) {
        state.liftEnded = true;
        // No post-idle fallback: it would silently replace the authored ending.
        CarryPoseLock::CaptureNow();
        state.endPose.Freeze();
        // A fixed grip is deliberately as old as the fix moment.
        if (!state.fixed && state.sampleAge > 0.25f) state.endPose.valid = false;
        // Hold the arm from the frozen sample on, so the lift's exit blend never pulls it away.
        if (state.endPose.valid) CarryHold::Hold();
        // Without a fix moment the root follows the grip from here to the attach, the way a fixed
        // target does from its fix (see Update): the pose is locked now (CaptureNow above).
        if (!state.fixed && state.endPose.valid)
            if (auto target = state.target.get(); Alive(target.get())) {
                state.follows = ResolveFollowRoot(target.get());
                logger::info(
                    "[CarryFix] lift ended request={} follow={} (no fix moment)", state.ticket.request, state.follows);
            }
        // Headtracking comes back at the attach (Attach), not here: TDM turning the carrier's spine
        // toward the camera while the pair still owns the target's placement swung the grip away
        // from the carried body (0.12.200 trace: Spine2 about 10 degrees out and back).
        const auto& r = state.endPose.relative;
        logger::info(
            "[CarryHandoff] frozen request={} valid={} samples={} sampleAge={:.4f} relative=({:.3f},{:.3f},{:.3f}) rotation=({:.3f},{:.3f},{:.3f};{:.3f},{:.3f},{:.3f};{:.3f},{:.3f},{:.3f})",
            state.ticket.request, state.endPose.valid, state.endPose.samples, state.sampleAge, r.translate.x,
            r.translate.y, r.translate.z, r.rotate.entry[0][0], r.rotate.entry[0][1], r.rotate.entry[0][2],
            r.rotate.entry[1][0], r.rotate.entry[1][1], r.rotate.entry[1][2], r.rotate.entry[2][0],
            r.rotate.entry[2][1], r.rotate.entry[2][2]);
        return;
    }
    if (pair.phase != PairedSession::Phase::kPlaying || !pair.initiator.entered || !pair.target.entered) return;
    auto carrier = state.carrier.get(), target = state.target.get();
    if (!Alive(carrier.get()) || !Alive(target.get())) return;
    auto* scene = target->Get3D(false);
    auto* carrierScene = carrier->Get3D(false);
    auto* anchor = carrierScene ? carrierScene->GetObjectByName(AnchorName()) : nullptr;
    auto* root = scene ? scene->GetObjectByName(TargetRoot()) : nullptr;
    RE::NiTransform local;
    if (!anchor || !anchor->AsNode() || !scene || !root || !ValidTransform(anchor->world) ||
        !ValidTransform(scene->world) || !RelativeRoot(scene, root, local))
        return;
    if (state.endPose.valid && (state.sampledCarrierScene.get() != carrierScene ||
                                   state.sampledTargetScene.get() != scene || state.sampledAnchor.get() != anchor)) {
        state.endPose.valid = false;
        state.endPose.Freeze();
        return;
    }
    if (state.fixed) {
        // The carrier keeps playing its lift after a fix: its lift-end arm pose is still sampled.
        CarryHold::CaptureLiftEnd();
        return;
    }
    const auto world = scene->world * local;
    const auto relative = CarryPlacement::NativeOffset(anchor->world, world);
    if (!ValidTransform(world) || !ValidTransform(relative) || Length(relative.translate) > 180) return;
    state.endPose.Sample(anchor->world, world);
    CarryHold::CaptureLiftEnd();
    state.sampleAge = 0;
    state.sampledCarrierScene.reset(carrierScene);
    state.sampledTargetScene.reset(scene);
    state.sampledAnchor.reset(anchor->AsNode());
    // The fix moment: the hook has locked the lift output at fixAt, and this sample of the drawn pose is
    // the grip the root keeps for the rest of the lift.
    if (const float at = CarryPoseLock::FixedAt(); at >= 0) {
        state.fixed = true;
        state.endPose.Freeze();
        state.follows = ResolveFollowRoot(target.get());
        const auto& r = state.endPose.relative;
        logger::info("[CarryFix] fixed request={} clipTime={:.3f} follow={} relative=({:.3f},{:.3f},{:.3f})",
            state.ticket.request, at, state.follows, r.translate.x, r.translate.y, r.translate.z);
    }
}
bool OwnsTargetRoot(PairedSession::Ticket ticket, RE::ActorHandle actor) {
    return ticket == state.ticket && actor == state.target &&
           (state.phase == Phase::kLifting || state.phase == Phase::kCarrying || state.phase == Phase::kPuttingDown);
}
void BeforeGameLoad() {
    input = false;
    drop = {};
    dropWatch = {};
    Stop("pre-load", false);
    CarryWeaponCover::Reset("load");
    CarryHold::Reset();
    CarryBody::Reset();
    RetryContactRecovery();
    loadedTarget = 0;
    loadedContacts = {};
}
void OnGameLoad() {
    dropWatch = {};
    PrecisionLink::ReleaseAll();
    // Also release retained old 3D if no pre-load callback reached this session.
    CarryWeaponCover::Reset("load");
    CarryHold::Reset();
    CarryBody::Reset();
    CarryAttachment::End(state.attachment);
    RevealTransferScene();
    hitMailbox.Reset();
    drop = {};
    transferSignal = 0;
    state.startControl.Release(false);
    ClearObservation();
    CarryPoseLock::Release("load");
    activeEntry = -1;
    input = false;
    state = {};
    contactRecovery = {};
    if (loadedTarget)
        if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(loadedTarget)) {
            actor->EnableAI(true);
            logger::info("[Carry] restored saved carried actor AI={:08X}", loadedTarget);
            // Saved mid-carry: the target was left at shoulder height. Drop it like a cancelled carry.
            if (!actor->IsDead()) {
                drop = {actor->GetHandle(), loadedTarget};
                drop.fromLoad = true;
                drop.handoff.Start();
                logger::info("[CarryDrop] queued reason=loaded-while-carried target={:08X}", loadedTarget);
            }
        }
    loadedTarget = 0;
    if (loadedContacts.actor && loadedContacts.owned)
        if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(loadedContacts.actor))
            contactRecovery = {actor->GetHandle(), RE::NiPointer<RE::TESObjectREFR>(actor), {loadedContacts.owned}};
    loadedContacts = {};
    RetryContactRecovery();
}
bool BlocksAction(RE::Actor* actor, int action) {
    if ((state.phase != Phase::kCarrying && state.phase != Phase::kPuttingDown && state.phase != Phase::kSuspended) ||
        !actor || (actor->GetHandle() != state.carrier && actor->GetHandle() != state.target))
        return false;
    // Only the carry itself starts its putdown Action on the carrier (BeginPutdown).
    return !(state.phase == Phase::kCarrying && action >= 0 && state.entry >= 0 &&
             state.entry < static_cast<int>(entries.size()) && action == entries[state.entry].putdown &&
             actor->GetHandle() == state.carrier);
}
void ObserveHit(RE::Actor* actor) {
    if (actor) hitMailbox.Hit(hitMailbox.Watch(actor->GetFormID()));
}
void BeforeUpdate() {
    RetryContactRecovery();
    if (state.phase == Phase::kIdle || !hitMailbox.watched[0].load()) return;
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto target = state.target.get();
    if (!target && state.targetId)
        if (auto* restored = RE::TESForm::LookupByID<RE::Actor>(state.targetId)) {
            state.target = restored->GetHandle();
            target = state.target.get();
        }
    if (!player || state.carrier != player->GetHandle()) {
        Stop("carrier-changed", false);
        return;
    }
    if (CheckInterruption(player, target.get())) return;
    if (state.phase != Phase::kCarrying && state.phase != Phase::kPuttingDown && state.phase != Phase::kSuspended)
        return;
    auto* ui = RE::UI::GetSingleton();
    const auto notice = transferSignal.exchange(0);
    const bool loading = ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
    if (state.phase == Phase::kSuspended) {
        HideTransferScene(target.get());
        return;
    }
    if (loading || hitMailbox.Owns(notice)) {
        Suspend("loading-or-cell-detach");
        return;
    }
    if (!player->Is3DLoaded() || !target->Is3DLoaded() || player->Get3D(false) != state.sampledCarrierScene.get() ||
        target->Get3D(false) != state.sampledTargetScene.get() ||
        target->GetCharController() != state.controller.get()) {
        Suspend("scene-recreated");
        return;
    }
    if (!CarryOwnership::SameScene(SceneOf(player), SceneOf(target.get())) ||
        !SharedPhysicsWorld(player, target.get()) || Length(player->GetPosition() - state.lastCarrier) > 150)
        Suspend("cell-or-position-changed");
}
// The carrier alone plays the putdown Action; the target stays attached until its CarryRelease moment.
// A rejected request (first person, cooldown, conditions) keeps the carry.
bool BeginPutdown() {
    const int action = entries[state.entry].putdown;
    if (!Actions::Runner(action).Request()) {
        logger::info("[CarryPutdown] '{}' request rejected; still carrying", Actions::Spec(action).id);
        return false;
    }
    state.phase = Phase::kPuttingDown;
    state.putdownAge = 0;
    headtracking.Acquire();
    // Hand the held arm to the putdown clip over its own entry blend, never to locomotion first.
    CarryHold::Release(false, Actions::Spec(action).enterBlend);
    logger::info("[CarryPutdown] '{}' started; target attached until CarryRelease", Actions::Spec(action).id);
    return true;
}
void Update(float delta) {
    // Again after the engine's player update (the first run is before it, also on paused frames):
    // a hit, knockdown or load notice that arrived during that update ends or suspends the carry
    // before this frame's steps use it.
    BeforeUpdate();
    AdvanceDrop(delta);
    TraceDropWatch(delta);
    CarryWeaponCover::Maintain();
    const bool pressed = input.exchange(false);
    if (state.phase == Phase::kIdle || !std::isfinite(delta) || delta <= 0) return;
    if (state.phase == Phase::kSuspended) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto target = state.target.get();
        if (player && target) ContinueTransfer(player, target.get(), delta);
        return;
    }
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!Alive(player)) {
        Stop("carrier-unavailable");
        return;
    }
    auto target = state.target.get();
    if (!Alive(target.get()) || state.carrier != player->GetHandle()) {
        logger::info("[Carry] participant invalid target={:08X} loaded={} dead={} ragdoll={} knock={} carrierSame={}",
            target ? target->GetFormID() : 0, target && target->Is3DLoaded(), target && target->IsDead(),
            target && target->IsInRagdollState(),
            target ? static_cast<int>(target->AsActorState()->GetKnockState()) : -1,
            state.carrier == player->GetHandle());
        Stop("participant-unavailable", false);
        return;
    }
    const auto carrierScene = SceneOf(player), targetScene = SceneOf(target.get());
    if (!CarryOwnership::SameScene(carrierScene, targetScene)) {
        logger::info("[Carry] scene mismatch cells={:08X}/{:08X} worlds={:08X}/{:08X} interior={}/{}",
            carrierScene.cell, targetScene.cell, carrierScene.world, targetScene.world, carrierScene.interior,
            targetScene.interior);
        Stop("different-scene", false);
        return;
    }
    if (carrierScene.cell != state.lastCarrierCell || targetScene.cell != state.lastTargetCell) {
        if (carrierScene.cell != targetScene.cell)
            logger::info("[Carry] exterior boundary allowed cells={:08X}/{:08X} world={:08X}", carrierScene.cell,
                targetScene.cell, carrierScene.world);
        state.lastCarrierCell = carrierScene.cell;
        state.lastTargetCell = targetScene.cell;
    }
    state.age += delta;
    state.sampleAge += delta;
    const auto pair = PairedSession::ReadStatus();
    if (state.phase == Phase::kLifting) {
        // The carry key does nothing until the lift ends; a press here is dropped, not kept for the
        // putdown. It used to cancel the lift: a second E just after the Pounding carry QTE (whose
        // result starts the lift 0.2 s after the press) freed the grappled NPC, who attacked.
        Sample();
        if (pair.rootHandoffSuppressed) {
            Stop("lift-transferred-to-another-action");
            return;
        }
        // weapon="sheathe": once the carrier plays the lift, its weapon goes away with no animation.
        CarryWeaponCover::Lifting(player,
            pair.ticket == state.ticket && pair.phase == PairedSession::Phase::kPlaying && pair.initiator.entered);
        if (state.endPose.frozen && !state.endPose.valid) {
            Stop("missing-final-pair-pose");
            return;
        }
        // From the fix moment (or the lift's end without one) the root stays on the carrier bone until
        // the attach, also between the lift's end and the pair's end: a scene placement there
        // (HoldEndPose) is overwritten by the pair before it is drawn, so the body stood still while the
        // carrier's exit blend moved the grip, then jumped 21.65 at the attach (0.12.200 trace). The
        // root written into the animation output is drawn; the last one stays in the locked pose for
        // the attach. HoldEndPose remains for a target whose animation root is not targetRoot.
        const bool following = state.follows;
        if (following) FollowRoot(player, target.get());
        const auto outcome =
            CarryOwnership::Completed(pair.ticket == state.ticket, pair.phase == PairedSession::Phase::kIdle,
                pair.lastEnd == PairedSession::EndReason::kCompleted ||
                    pair.lastEnd == PairedSession::EndReason::kCancelled ||
                    pair.lastEnd == PairedSession::EndReason::kMovementRecovery,
                pair.initiator.entered, pair.target.entered, pair.initiator.exited, pair.target.exited,
                pair.initiator.graphStateRestored, pair.target.graphStateRestored);
        if (outcome == CarryOwnership::PairResult::kFailure || state.age > 18) {
            Stop("pair-did-not-complete");
            return;
        }
        if (outcome == CarryOwnership::PairResult::kWait) {
            if (state.liftEnded && !following && !HoldEndPose(player, target.get())) Stop("end-pose-hold-invalid");
            return;
        }
        if (!Attach(player, target.get())) {
            Stop("attach-context-invalid");
            return;
        }
    }
    if (pair.phase != PairedSession::Phase::kIdle) {
        Stop("another-pair-started", false);
        return;
    }
    // Walking, sprinting, jumping, stagger and ordinary carrier actions do not
    // expire a carry. Hit/death/ragdoll and the putdown own its lifetime.
    auto* cc = target->GetCharController();
    auto* root = player->Get3D(false);
    auto* bone = root ? root->GetObjectByName(AnchorName()) : nullptr;
    auto* scene = target->Get3D(false);
    auto* targetRoot = scene ? scene->GetObjectByName(TargetRoot()) : nullptr;
    const auto current = player->GetPosition();
    if (!cc || cc != state.controller.get() || !bone || !scene || !targetRoot ||
        !CarryAttachment::Matches(state.attachment, player, target.get(), bone ? bone->AsNode() : nullptr) ||
        !Finite(current) || Length(current - state.lastCarrier) > 150) {
        Stop("anchor-or-controller-changed");
        return;
    }
    state.distance += Length(current - state.lastCarrier);
    state.lastCarrier = current;
    bool putdown = false;
    if (pressed && state.phase == Phase::kCarrying) {
        if (player->IsInMidair())
            logger::info("[Carry] putdown blocked while airborne; still carrying, press again after landing");
        else
            putdown = true;
    }
    const auto wanted = bone->world.translate;
    RE::NiTransform rootInScene;
    if (!Finite(wanted) || Length(wanted - current) > 180 || !ValidTransform(bone->world) ||
        !ValidTransform(scene->world) || !RelativeRoot(scene, targetRoot, rootInScene)) {
        Stop("invalid-anchor");
        return;
    }
    // Diagnostic expectation only. The native controller writes the reference
    // and local transform; the carry module supplies its missing update tick.
    const auto desiredScene = CarryPlacement::NativeScene(bone->world, state.syncOffset, scene->world.scale);
    const auto& actorPosition = desiredScene.translate;
    if (!Finite(actorPosition) || !ValidTransform(desiredScene) || Length(actorPosition - current) > 220) {
        logger::warn(
            "[Carry] placement rejected age={:.3f} actor=({:.3f},{:.3f},{:.3f}) carrier=({:.3f},{:.3f},{:.3f}) rootLocal=({:.3f},{:.3f},{:.3f}) scene=({:.3f},{:.3f},{:.3f})",
            state.age, actorPosition.x, actorPosition.y, actorPosition.z, current.x, current.y, current.z,
            rootInScene.translate.x, rootInScene.translate.y, rootInScene.translate.z, scene->world.translate.x,
            scene->world.translate.y, scene->world.translate.z);
        Stop("invalid-root-offset");
        return;
    }
    if (!CarryAttachment::Pulse(state.attachment, player, target.get(), bone->AsNode())) {
        Stop("native-sync-refresh-unavailable", false);
        return;
    }
    // Native mount sync moves only the scene/reference. Its carried collision
    // capsule must follow too, with actual contacts disabled before relocation.
    // This also prevents an AI update from restoring the old physical position.
    if (!CarryPhysics::Follow(cc, target->GetPosition())) {
        Stop("carry-physics-unavailable");
        return;
    }
    // Physics target: begin after one pulse has been drawn, then step after the attachment moved this
    // frame; a safety stop falls back to the pose and starts the physics again after a wait.
    if (state.bodyRestartIn > 0 && (state.bodyRestartIn -= delta) <= 0) {
        state.bodyRestartIn = -1;
        if (state.phase == Phase::kCarrying) state.bodyPending = true;
    }
    if (state.bodyPending && state.attachment.ticks > 1) {
        state.bodyPending = false;
        const auto& lift = Actions::Spec(entries[state.entry].action);
        const bool restart = state.bodyRestarts > 0;
        const auto started = CarryBody::Begin(state.carrier, state.target, lift.paired.targetRig,
            lift.clips.empty() ? std::string_view() : std::string_view(lift.clips.front().file),
            entries[state.entry].part, AnchorName(), state.ticket.request);
        if (started == CarryBody::Start::kTargetSimulating) {
            // The target's own ragdoll never stopped simulating (grabbed while it was getting up): a
            // carry in the lift pose fought the engine's physics, and the putdown lost the target
            // (0.12.216). It is let go to the engine, as a ragdolled target is.
            Stop("target-physics-simulating", false);
            return;
        }
        if (started == CarryBody::Start::kStarted) {
            state.bodyAge = 0;
            state.bodyFailedStarts = 0;
            if (restart) logger::info("[Carry] physics restarted (attempt {})", state.bodyRestarts);
        } else if (restart && ++state.bodyFailedStarts < maxBodyRetries) {
            state.bodyBackoff = std::min(maxBodyBackoff, std::max(firstBodyBackoff, state.bodyBackoff * 2));
            state.bodyRestartIn = state.bodyBackoff;
            logger::warn("[Carry] physics restart could not start; next try in {:.1f}s", state.bodyRestartIn);
        } else if (restart)
            logger::warn(
                "[Carry] physics restart failed {} times; continuing with the lift-end pose", state.bodyFailedStarts);
    }
    state.bodyAge += delta;
    if (CarryBody::Update(delta) == CarryBody::State::kFaulted) {
        CarryBody::End(false);
        if (state.phase == Phase::kCarrying) {
            // Back to the locked pose now; physics again once the wait is over.
            state.bodyBackoff = state.bodyAge < 2 && state.bodyBackoff > 0
                                    ? std::min(maxBodyBackoff, state.bodyBackoff * 2)
                                    : firstBodyBackoff;
            state.bodyRestartIn = state.bodyBackoff;
            ++state.bodyRestarts;
            logger::warn("[Carry] physics target stopped after {:.1f}s; restarting from the lift-end pose in {:.1f}s "
                         "(attempt {})",
                state.bodyAge, state.bodyRestartIn, state.bodyRestarts);
        } else
            logger::warn("[Carry] physics target stopped; continuing with the lift-end pose");
    }
    const float actorError = Length(target->GetPosition() - actorPosition);
    const float sceneError = Length(scene->world.translate - actorPosition);
    // An installed controller alone is not success. Release AI/physics if it
    // cannot produce the expected transform, instead of freezing the target.
    const bool finite = std::isfinite(actorError) && std::isfinite(sceneError);
    state.syncFailureTime = finite && actorError <= 16 && sceneError <= 16 ? 0 : state.syncFailureTime + delta;
    if (!finite || state.syncFailureTime >= 0.5f) {
        logger::warn("[Carry] native sync ineffective ticks={} actorError={:.3f} sceneError={:.3f}; restoring target",
            state.attachment.ticks, actorError, sceneError);
        Stop("native-sync-did-not-update", false);
        return;
    }
    if (putdown) BeginPutdown();
    if (state.phase == Phase::kPuttingDown) {
        state.putdownAge += delta;
        // The CarryRelease moment drops the target; so does a clip that ends (or never reaches it) first.
        const auto& runner = Actions::Runner(entries[state.entry].putdown);
        const char* reason = runner.CarryReleaseDue()                            ? "putdown"
                             : runner.GetState() != ActionRunner::State::kActive ? "putdown-clip-ended"
                             : state.putdownAge > 20                             ? "putdown-timeout"
                                                                                 : nullptr;
        if (reason) {
            logger::info("[CarryPutdown] release reason={} after {:.3f}s", reason, state.putdownAge);
            CancelCarry(reason);
            return;
        }
    }
    // The ground in front of the carrier, where a release places the target.
    state.trace -= delta;
    if (state.trace <= 0) {
        state.trace = state.age < 2 ? 0.1f : 1.0f;
        RE::NiPoint3 ground;
        if (CarryGround::InFront(player, ground)) {
            state.ground = ground;
            state.hasGround = true;
        }
    }
}
}
