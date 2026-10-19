#pragma once
#include "runtime/PairedSession.h"
namespace RE {
class Actor;
}

namespace AnimationInjector::Carry {
// XML-configured carry following a completed player-initiated pair.
// Main-thread lifecycle; input only queues intent. No actor/3D raw pointer is retained.
bool CanBegin(int action, RE::Actor* carrier, RE::Actor* target);
void PairStarted(int action, const PairedSession::Status& pair);
enum class StartGate { kWaiting, kReady, kFailed };
// Native placement waits for any queued steering reservation.
StartGate PrepareStart(PairedSession::Ticket ticket);
// Main-thread bridge from the lift into the common native Start: owned scene
// references, never retained Havok generator pointers.
struct NativeStartBridge {
    struct Role {
        RE::ActorHandle actor;
        RE::NiPointer<RE::NiAVObject> scene;
        RE::BSFixedString generatorName;
    };
    PairedSession::Ticket ticket;
    std::array<Role, 2> roles;
    std::uint64_t camera = 0;
};
bool ReadNativeStartBridge(RE::ActorHandle support, RE::ActorHandle leader,
    const std::array<RE::BSFixedString, 2>& generators, NativeStartBridge& output);
// Transfer the camera lease exactly once, after common Start validation
// succeeds. The carry input/physics leases remain carry-owned.
bool CommitNativeStartBridge(const NativeStartBridge& bridge);
// Unsupported/rejected starts must not leave a detached pose held indefinitely.
void AbandonNativeStartBridge(PairedSession::Ticket ticket);
// Hook observations only record identity and exit intent. No scene writes here.
void ObserveClip(PairedSession::Ticket ticket, RE::ActorHandle actor, const RE::hkbContext& context,
    const RE::BSSynchronizedClipGenerator& generator, bool exiting);
void ObserveExitBlend(const RE::hkbContext& context);
void ObserveEndEvent(PairedSession::Ticket ticket, RE::ActorHandle actor);
// Called around the engine update; removes our transient visual layer before
// native evaluation and copies the last playing lift pose.
void Sample();
// Release only the startup steering lease; retain the frozen target pose and pair ownership.
void ReleaseMovementControl(PairedSession::Ticket ticket, RE::ActorHandle actor);
bool OwnsTargetRoot(PairedSession::Ticket ticket, RE::ActorHandle actor);
void InstallSerialization();
void Register();
// Main thread: ends the carry; a held target is dropped (the path of a hit carrier). No carry: nothing.
void Cancel(const char* reason);
// A carry is declared: the input dispatch hook must run for ClaimInput.
bool Enabled();
// Input dispatch, before PlayerControls: the putdown key while a carry is active is consumed,
// so the game does not act on it too (E = Activate). Returns whether the event was claimed.
bool ClaimInput(const RE::ButtonEvent& event, bool allow);
void Update(float delta);
// Main-thread pre-engine maintenance, including paused loading-screen frames.
void BeforeUpdate();
// Hit events only enqueue a generation-tagged interruption for the current carry.
void ObserveHit(RE::Actor* actor);
void BeforeGameLoad();
void OnGameLoad();
// A carry owns its carrier and target; only the carry itself may start its putdown Action
// (`action`) on the carrier. action < 0 asks about any action.
bool BlocksAction(RE::Actor* actor, int action = -1);
}
