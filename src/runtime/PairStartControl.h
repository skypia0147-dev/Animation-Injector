#pragma once
#include "PCH.h"
#include <memory>

namespace AnimationInjector::PairStartControl {
enum class Readiness { kWaiting, kReady, kFailed };
struct YawRequest;
// TDM's player yaw (RequestYawControl, 0.12.279): while held, TDM neither faces the crosshair (blocking,
// bashing, aiming) nor steers the body toward movement, so only the engine and the pair's authored rotation
// turn the player. Asked at once when called on the main thread (TDM's update reads it there), else as a game
// task. Release returns only what this lease was given (AlreadyGiven/AlreadyTaken are not ours to return).
// Without TDM it does nothing.
struct YawLease {
    std::shared_ptr<YawRequest> request;
    void Acquire();
    Readiness Ready() const;  // kReady without TDM
    void Release();
};
// Actor-update lease; optional TDM calls run in scheduled game tasks.
// The native pair may rotate normally; input steering waits.
// Explicit release is required before discarding a carry/session state.
struct Lease {
    struct Role {
        RE::ActorHandle actor;
        RE::NiPointer<RE::NiAVObject> scene;
        bool rotationChanged = false;
    };
    std::array<Role, 2> roles;
    bool active = false;
    std::uint64_t cameraToken = 0;
    YawLease yaw;
    // true accepts the reservation. Check Ready before native placement/start.
    bool Acquire(RE::Actor* initiator, RE::Actor* target);
    Readiness Ready() const;
    // false drops old-save actor leases, but still returns our optional API lease.
    void Release(bool restoreActors = true);
    // Transfer the moving participant to locomotion, without restoring its old rotation mode.
    void ReleaseForMovement(RE::ActorHandle actor);
};
struct HeadtrackRequest;
// TDM's player head tracking turns the head, neck and spine toward the camera or a target, which
// bends everything below them (a carrying arm) away from the paired clip. Carry holds it off for
// the lift and putdown pairs. Calls run as game tasks like the yaw lease; without TDM nothing
// happens. Release is idempotent.
struct HeadtrackingLease {
    std::shared_ptr<HeadtrackRequest> request;
    void Acquire();
    void Release();
};
}
