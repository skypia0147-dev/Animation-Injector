#include "runtime/PairStartControl.h"
#include "runtime/PairStartCamera.h"
#include "compat/TrueDirectionalMovementAPI.h"
#include "runtime/MainThread.h"
#include <mutex>

namespace AnimationInjector::PairStartControl {
struct YawRequest {
    std::mutex mutex;
    Readiness ready = Readiness::kWaiting;
    bool cancelled = false, owned = false;
};
namespace {
std::mutex apiMutex;
const RE::BSFixedString& Rotation() {
    static const RE::BSFixedString name("bAllowRotation");
    return name;
}
TDM_API::IVTDM2* TDM() {
    static auto* api = []() -> TDM_API::IVTDM2* {
        auto module = GetModuleHandleW(L"TrueDirectionalMovement.dll");
        if (!module) return nullptr;
        const auto request = reinterpret_cast<TDM_API::_RequestPluginAPI>(GetProcAddress(module, "RequestPluginAPI"));
        return request ? static_cast<TDM_API::IVTDM2*>(request(TDM_API::InterfaceVersion::V2)) : nullptr;
    }();
    return api;
}
void AcquireYaw(const std::shared_ptr<YawRequest>& request) {
    std::lock_guard lock(request->mutex);
    if (request->cancelled) return;
    // TDM stores its API construction thread, not a scheduler affinity. SKSE
    // game tasks can execute on different OS threads across frames.
    std::lock_guard apiLock(apiMutex);
    const auto result = TDM()->RequestYawControl(SKSE::GetPluginHandle(), 0);
    request->owned = result == TDM_API::APIResult::OK;
    // AlreadyGiven: this plugin holds it already (a carry lease and the pair's own); ready, but not ours to return.
    request->ready =
        request->owned || result == TDM_API::APIResult::AlreadyGiven ? Readiness::kReady : Readiness::kFailed;
    logger::debug("[PairStartControl] yaw request completed result={} thread={}", static_cast<unsigned>(result),
        GetCurrentThreadId());
}
void ReleaseYaw(const std::shared_ptr<YawRequest>& request) {
    std::lock_guard lock(request->mutex);
    if (!request->owned) return;
    std::lock_guard apiLock(apiMutex);
    const auto result = TDM()->ReleaseYawControl(SKSE::GetPluginHandle());
    request->owned = false;
    logger::debug("[PairStartControl] returned yaw lease result={} thread={}", static_cast<unsigned>(result),
        GetCurrentThreadId());
}
void CancelYaw(const std::shared_ptr<YawRequest>& request) {
    {
        std::lock_guard lock(request->mutex);
        request->cancelled = true;
        if (!request->owned) return;
    }
    // On the main thread return it now, so a lease asked right after (a connected pair, a carry lift) is granted
    // anew instead of finding this one about to go.
    if (MainThread::IsCurrent())
        ReleaseYaw(request);
    else
        SKSE::GetTaskInterface()->AddTask([request] { ReleaseYaw(request); });
}
}
bool Lease::Acquire(RE::Actor* self, RE::Actor* target) {
    if (active) return self && target && roles[0].actor == self->GetHandle() && roles[1].actor == target->GetHandle();
    if (!self || !target || self == target || !self->Is3DLoaded() || !target->Is3DLoaded() || !self->Get3D(false) ||
        !target->Get3D(false) || self->IsDead() || target->IsDead() || self->IsInRagdollState() ||
        target->IsInRagdollState()) {
        logger::warn("[PairStartControl] participant unavailable; start not reserved");
        return false;
    }
    // RequestYawControl resets TDM's pending desired angle. Never SetPlayerYaw:
    // only engine/authored rotation may drive the body after native Start.
    if ((self->IsPlayerRef() || target->IsPlayerRef()) && TDM()) {
        if (!SKSE::GetTaskInterface()) {
            logger::warn("[PairStartControl] task interface unavailable");
            return false;
        }
        // Retain the reservation until TDM grants or rejects it.
        yaw.Acquire();
    }
    const std::array<RE::Actor*, 2> actors{self, target};
    active = true;
    if (self->IsPlayerRef() || target->IsPlayerRef()) {
        cameraToken = PairStartCamera::Acquire(self, target);
        if (!cameraToken) {
            Release();
            return false;
        }
    }
    for (unsigned i = 0; i < 2; ++i) {
        auto& role = roles[i];
        auto* actor = actors[i];
        role.actor = actor->GetHandle();
        role.scene.reset(actor->Get3D(false));
        bool allowed = false;
        if (actor->GetGraphVariableBool(Rotation(), allowed) && allowed) {
            role.rotationChanged = actor->SetGraphVariableBool(Rotation(), false);
            if (!role.rotationChanged) {
                Release();
                return false;
            }
        }
    }
    if (Ready() == Readiness::kFailed) {
        Release();
        return false;
    }
    logger::info("[PairStartControl] reserved actors={:08X}/{:08X} graphOwned={}/{} readiness={}", self->GetFormID(),
        target->GetFormID(), roles[0].rotationChanged, roles[1].rotationChanged, static_cast<unsigned>(Ready()));
    return true;
}
Readiness Lease::Ready() const {
    if (!active) return Readiness::kFailed;
    return yaw.Ready();
}
void YawLease::Acquire() {
    auto* tasks = SKSE::GetTaskInterface();
    if (request || !TDM() || !tasks) return;
    request = std::make_shared<YawRequest>();
    // On the main thread TDM's own update cannot race the call, and a pair that starts in this same frame is
    // covered from its first frame; elsewhere (an input or graph callback) it waits for the game task.
    if (MainThread::IsCurrent())
        AcquireYaw(request);
    else {
        logger::debug("[PairStartControl] queue yaw request caller={} apiCreatedOn={} (diagnostic only)",
            GetCurrentThreadId(), TDM()->GetTDMThreadId());
        tasks->AddTask([request = request] { AcquireYaw(request); });
    }
}
Readiness YawLease::Ready() const {
    if (!request) return Readiness::kReady;
    std::lock_guard lock(request->mutex);
    return request->ready;
}
void YawLease::Release() {
    if (!request) return;
    CancelYaw(request);
    request.reset();
}
void Lease::ReleaseForMovement(RE::ActorHandle actor) {
    for (auto& role : roles)
        if (role.actor == actor) role.rotationChanged = false;
    Release();
}
struct HeadtrackRequest {
    std::mutex mutex;
    bool cancelled = false, owned = false;
};
void HeadtrackingLease::Acquire() {
    auto* tasks = SKSE::GetTaskInterface();
    if (request || !TDM() || !tasks) return;
    request = std::make_shared<HeadtrackRequest>();
    tasks->AddTask([request = request] {
        std::lock_guard lock(request->mutex);
        if (request->cancelled) return;
        std::lock_guard apiLock(apiMutex);
        const auto result = TDM()->RequestDisableHeadtracking(SKSE::GetPluginHandle());
        request->owned = result == TDM_API::APIResult::OK || result == TDM_API::APIResult::AlreadyGiven;
        logger::debug("[PairStartControl] headtracking disable result={} owned={}", static_cast<unsigned>(result),
            request->owned);
    });
}
void HeadtrackingLease::Release() {
    if (!request) return;
    auto held = std::move(request);
    request.reset();
    {
        std::lock_guard lock(held->mutex);
        held->cancelled = true;  // A request still queued behind this call never runs.
        if (!held->owned) return;
    }
    SKSE::GetTaskInterface()->AddTask([held] {
        std::lock_guard lock(held->mutex);
        if (!held->owned) return;
        std::lock_guard apiLock(apiMutex);
        const auto result = TDM()->ReleaseDisableHeadtracking(SKSE::GetPluginHandle());
        held->owned = false;
        logger::debug("[PairStartControl] headtracking restored result={}", static_cast<unsigned>(result));
    });
}
void Lease::Release(bool restoreActors) {
    PairStartCamera::Release(cameraToken);
    cameraToken = 0;
    if (restoreActors)
        for (auto& role : roles) {
            if (!role.rotationChanged) continue;
            auto actor = role.actor.get();
            if (!actor || !actor->Is3DLoaded() || actor->Get3D(false) != role.scene.get()) continue;
            bool current = true;
            if (actor->GetGraphVariableBool(Rotation(), current) && !current)
                actor->SetGraphVariableBool(Rotation(), true);
        }
    yaw.Release();
    roles = {};
    active = false;
}
}
