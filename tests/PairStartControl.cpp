#include "runtime/PairStartControl.h"
#include "runtime/PairStartCamera.h"
#include "compat/TrueDirectionalMovementAPI.h"
#include "runtime/MainThread.h"
#include <cstdlib>
#include <iostream>
namespace {
bool installed = true;
using namespace TDM_API;
struct API final : IVTDM2 {
    unsigned owner = 0, acquired = 0, released = 0, yawWrites = 0, outsideTask = 0;
    // This is the construction thread, and never follows task workers.
    unsigned long GetTDMThreadId() const noexcept override { return 12828; }
    bool GetDirectionalMovementState() const noexcept override { return true; }
    bool GetTargetLockState() const noexcept override { return false; }
    ActorHandle GetCurrentTarget() const noexcept override { return {}; }
    APIResult RequestDisableDirectionalMovement(PluginHandle) noexcept override { return APIResult::MustKeep; }
    APIResult RequestDisableHeadtracking(PluginHandle) noexcept override { return APIResult::MustKeep; }
    PluginHandle GetDisableDirectionalMovementOwner() const noexcept override { return 0; }
    PluginHandle GetDisableHeadtrackingOwner() const noexcept override { return 0; }
    APIResult ReleaseDisableDirectionalMovement(PluginHandle) noexcept override { return APIResult::NotOwner; }
    APIResult ReleaseDisableHeadtracking(PluginHandle) noexcept override { return APIResult::NotOwner; }
    APIResult RequestYawControl(PluginHandle who, float) noexcept override {
        if (!gameTaskActive) ++outsideTask;
        if (owner) return owner == who ? APIResult::AlreadyGiven : APIResult::AlreadyTaken;
        owner = who;
        ++acquired;
        return APIResult::OK;
    }
    APIResult SetPlayerYaw(PluginHandle, float) noexcept override {
        ++yawWrites;
        return APIResult::OK;
    }
    APIResult ReleaseYawControl(PluginHandle who) noexcept override {
        if (!gameTaskActive) ++outsideTask;
        if (owner != who) return APIResult::NotOwner;
        owner = 0;
        ++released;
        return APIResult::OK;
    }
} api;
void* Request(InterfaceVersion v) { return v == InterfaceVersion::V2 ? &api : nullptr; }
void Check(bool ok, const char* text) {
    if (!ok) {
        std::cerr << text << '\n';
        std::exit(1);
    }
}
using AnimationInjector::PairStartControl::Lease;
using AnimationInjector::PairStartControl::Readiness;
bool Reserve(Lease& lease, RE::Actor* a, RE::Actor* b) {
    if (!lease.Acquire(a, b)) return false;
    SKSE::taskInterface.Drain();
    return lease.Ready() == Readiness::kReady;
}
void Release(Lease& lease) {
    lease.Release();
    SKSE::taskInterface.Drain(16056);
}
}
void* GetModuleHandleW(const wchar_t*) { return installed ? &api : nullptr; }
// The test's own thread; MainThread::IsCurrent() is false until a case captures it.
std::uint32_t AnimationInjector::MainThread::CurrentId() { return 31337; }
void* GetProcAddress(void*, const char*) { return reinterpret_cast<void*>(&Request); }
namespace AnimationInjector::PairStartCamera {
std::uint64_t cameraOwner = 0, cameraSerial = 0;
std::uint64_t Acquire(RE::Actor*, RE::Actor*) {
    if (cameraOwner) return 0;
    return cameraOwner = ++cameraSerial;
}
void Release(std::uint64_t token) {
    if (cameraOwner == token) cameraOwner = 0;
}
}
int main(int argc, char**) {
    installed = argc == 1;
    RE::Actor a(1), b(2);
    a.player = true;
    Lease lease;
    Check(!lease.Acquire(nullptr, &b) && !lease.active, "missing actor rejected");
    if (installed) {
        api.owner = 42;
        Check(!Reserve(lease, &a, &b), "other owner rejects request");
        Release(lease);
        Check(api.owner == 42 && a.rotation && b.rotation, "other owner preserved");
        api.owner = 0;
    }
    Check(Reserve(lease, &a, &b) && lease.active && !a.rotation && !b.rotation, "locks steering");
    Check(lease.Acquire(&a, &b), "same lease reused");
    Check(!lease.Acquire(&b, &a), "different participants cannot adopt lease");
    for (unsigned frame = 0; frame < 20; ++frame)
        Check(lease.Ready() == Readiness::kReady && !a.rotation && !b.rotation, "native activation retains lease");
    Release(lease);
    Check(!lease.active && a.rotation && b.rotation && api.owner == 0, "completion restores controls");
    const auto releases = api.released;
    Release(lease);
    Check(api.released == releases, "duplicate release inert");
    if (installed) {
        // Match the report: 12828 input, 46032 Actor update, changing game-task
        // workers 15096/65132/16056. OS thread identity is not API eligibility.
        currentThread = 12828;
        Check(Reserve(lease, &a, &b), "lift accepted");
        Release(lease);
        for (auto worker : {15096ul, 65132ul, 16056ul}) {
            currentThread = 46032;
            const auto calls = api.acquired;
            Check(lease.Acquire(&a, &b) && lease.Ready() == Readiness::kWaiting, "lower request retained");
            Check(lease.Acquire(&a, &b) && SKSE::taskInterface.tasks.size() == 1 && api.acquired == calls,
                "samples enqueue once");
            for (unsigned i = 0; i < 3; ++i) Check(lease.Ready() == Readiness::kWaiting, "native placement waits");
            SKSE::taskInterface.Drain(worker);
            Check(lease.Ready() == Readiness::kReady && api.owner == 7,
                "same G request proceeds on changing task worker");
            lease.Release();
            Check(a.rotation && b.rotation, "worker cleanup restores graph");
            SKSE::taskInterface.Drain(worker + 1);
            Check(api.owner == 0, "return on a different worker");
        }
        Check(lease.Acquire(&a, &b), "queue before cancellation");
        const auto calls = api.acquired;
        Release(lease);
        Check(api.acquired == calls && api.owner == 0, "cancelled task never acquires");
        Check(lease.Acquire(&a, &b), "old queued request");
        lease.Release();
        Check(Reserve(lease, &a, &b), "new reservation survives old cancelled task");
        Release(lease);
        b.writeFails = true;
        Check(!lease.Acquire(&a, &b), "partial graph failure with queued API");
        SKSE::taskInterface.Drain();
        Check(api.owner == 0 && a.rotation, "partial failure cancels API");
        b.writeFails = false;
        Check(lease.Acquire(&a, &b), "pending before load");
        const auto reads = RE::handleReads;
        lease.Release(false);
        SKSE::taskInterface.Drain();
        Check(RE::handleReads == reads && api.owner == 0, "load cancels without old handles");
        a.rotation = b.rotation = true;
        Check(Reserve(lease, &a, &b), "granted before load");
        lease.Release(false);
        SKSE::taskInterface.Drain();
        Check(api.owner == 0, "load returns granted task lease");
        a.rotation = b.rotation = true;
        Check(lease.Acquire(&a, &b), "queue before ownership conflict");
        api.owner = 42;
        SKSE::taskInterface.Drain();
        Check(lease.Ready() == Readiness::kFailed, "queued conflict reported");
        Release(lease);
        Check(api.owner == 42 && a.rotation && b.rotation, "conflict leaves external owner intact");
        api.owner = 0;
    }
    // Recovery must release steering before the source blend completes, and must
    // not restore the player's old animation-rotation mode over locomotion.
    Check(Reserve(lease, &a, &b), "before movement recovery");
    lease.ReleaseForMovement(a.GetHandle());
    SKSE::taskInterface.Drain();
    Check(!lease.active && !a.rotation && b.rotation && api.owner == 0,
        "movement release preserves locomotion and restores partner");
    Release(lease);
    Check(!a.rotation, "late completion cannot restore old player rotation");
    a.rotation = true;
    a.rotation = false;
    b.rotation = true;
    Check(Reserve(lease, &a, &b), "preexisting lock accepted");
    Release(lease);
    Check(!a.rotation && b.rotation, "preexisting rotation preserved");
    a.rotation = true;
    b.writeFails = true;
    Check(!lease.Acquire(&a, &b), "partial graph failure");
    SKSE::taskInterface.Drain();
    Check(a.rotation && api.owner == 0 && !lease.active, "partial failure unwinds");
    b.writeFails = false;
    Check(Reserve(lease, &a, &b), "before 3D replacement");
    a.scene = &a.replacement;
    Release(lease);
    Check(!a.rotation && b.rotation, "replacement scene never restored");
    a.scene = &a.object;
    a.rotation = true;
    Check(Reserve(lease, &a, &b), "before unload");
    a.loaded = false;
    Release(lease);
    Check(!a.rotation && b.rotation && api.owner == 0, "unloaded actor skipped");
    Check(api.yawWrites == 0 && api.outsideTask == 0, "no forced yaw or API writes outside game tasks");
    Check(api.acquired == api.released && SKSE::taskInterface.tasks.empty(), "all owned requests returned");
    Check(AnimationInjector::PairStartCamera::cameraOwner == 0, "camera protection released on every lifecycle exit");
    // 0.12.279: a pair's own yaw lease. On the main thread TDM is asked and answered at once (a pair starts in the
    // same frame); AlreadyGiven is ready but never returned by the lease that did not get it.
    {
        using AnimationInjector::PairStartControl::YawLease;
        namespace main = AnimationInjector::MainThread;
        YawLease yaw;
        if (installed) {
            main::detail::id.store(main::CurrentId());
            yaw.Acquire();
            Check(api.owner == 7 && yaw.Ready() == Readiness::kReady && SKSE::taskInterface.tasks.empty(),
                "the main thread asks TDM at once");
            YawLease again;
            again.Acquire();
            Check(again.Ready() == Readiness::kReady, "a lease this plugin already holds is ready");
            again.Release();
            Check(api.owner == 7, "a lease that was not given is not returned");
            yaw.Release();
            Check(api.owner == 0 && SKSE::taskInterface.tasks.empty(), "the main thread returns it at once");
            main::detail::id.store(0);
            yaw.Acquire();
            Check(
                yaw.Ready() == Readiness::kWaiting && SKSE::taskInterface.tasks.size() == 1, "elsewhere it is queued");
            SKSE::taskInterface.Drain();
            Check(yaw.Ready() == Readiness::kReady && api.owner == 7, "the queued request is granted");
            yaw.Release();
            SKSE::taskInterface.Drain();
            Check(api.owner == 0, "and returned by a game task");
        } else {
            yaw.Acquire();
            Check(!yaw.request && yaw.Ready() == Readiness::kReady, "without TDM nothing is asked");
        }
    }
    std::cout << "PASS: changing game-task workers, retained input, conflict/cancel/load/3D cleanup; TDM=" << installed
              << '\n';
}
