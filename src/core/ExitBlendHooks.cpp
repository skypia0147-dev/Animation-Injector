#include "PCH.h"
#include "core/ExitBlendHooks.h"
#include "core/ClipHooks.h"
#include "runtime/Carry.h"
#include "runtime/ActionRegistry.h"
#include "runtime/ActionRunner.h"
#include "runtime/ExitBlendMailbox.h"
#include "runtime/PairLocomotionStart.h"
#include "runtime/LocomotionDestinationWindow.h"
#include "runtime/InputSink.h"
#include "core/NodeFactory.h"
#include "core/HavokLayout.h"
#include "core/GraphInjector.h"
#include <atomic>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>

namespace AnimationInjector::ExitBlendHooks {
namespace {
struct Request {
    RE::ActorHandle actor;
    int action = -1;
    std::uint64_t serial = 0;
    PairedSession::Ticket pair;
    bool paired = false;
};
std::mutex mutex;  // the mailbox, destination events and the locomotion destination
// Every blending transition of every actor activates through the hooks below, on the animation threads.
// The exit names are written while graphs load and read at each activation: a shared lock and one hash
// lookup instead of the global lock and a tree search (code review 2026-09-30 R-18).
struct NameHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view text) const noexcept { return std::hash<std::string_view>{}(text); }
};
std::shared_mutex exitsLock;
// Exit effect name -> action, or kSessionPair / kSessionPairLoss for the shared pair exits.
std::unordered_map<std::string, int, NameHash, std::equal_to<>> exits;
constexpr int kSessionPair = -2;
// The shared partner-loss exit (0.12.286): the survivor's blend when its partner dies or ragdolls.
constexpr int kSessionPairLoss = -3;
ExitBlendMailbox<Request> mailbox;
// The mailbox's held items, read without the lock: with none, an activation has nothing to clear.
std::atomic<std::size_t> mailboxItems{0};
// Clears the key's stale item. Caller holds `mutex`.
void DeactivateLocked(ExitBlendMailbox<Request>::Key key) {
    mailbox.Deactivate(key);
    mailboxItems.store(mailbox.Active(), std::memory_order_release);
}
std::atomic_bool movement{false};
std::map<std::string, int, std::less<>> destinationEvents;
struct DestinationOwner {
    RE::ActorHandle actor;
    PairedSession::Ticket ticket;
    bool operator==(const DestinationOwner&) const = default;
};
LocomotionDestinationWindow<DestinationOwner> destination;
RE::NiPointer<RE::NiAVObject> destinationScene;
bool Eligible(RE::Actor* actor) {
    auto* state = actor ? actor->AsActorState() : nullptr;
    auto* controls = RE::PlayerControls::GetSingleton();
    auto* map = RE::ControlMap::GetSingleton();
    auto* ui = RE::UI::GetSingleton();
    return actor && state && actor->IsPlayerRef() && actor->Is3DLoaded() && !actor->IsDead() &&
           !actor->IsInRagdollState() && !actor->IsOnMount() &&
           state->GetKnockState() == RE::KNOCK_STATE_ENUM::kNormal && !state->IsStaggered() &&
           !state->IsBleedingOut() && ui && !ui->GameIsPaused() && !ui->IsMenuOpen(RE::Console::MENU_NAME) &&
           controls && !controls->blockPlayerInput && map && map->IsMovementControlsEnabled();
}
bool ValidStatus(const PairedSession::Status& status, const DestinationOwner& owner) {
    return status.ticket == owner.ticket && !status.rootHandoffSuppressed &&
           (status.initiator.actor == owner.actor || status.target.actor == owner.actor) && status.initiator.entered &&
           status.target.entered &&
           (status.phase == PairedSession::Phase::kPlaying ||
               (status.phase == PairedSession::Phase::kStopping &&
                   (status.lastEnd == PairedSession::EndReason::kCompleted ||
                       status.lastEnd == PairedSession::EndReason::kMovementRecovery ||
                       status.lastEnd == PairedSession::EndReason::kParticipantUnavailable)));
}
// The pair this actor plays through a shared exit effect, when that pair authored an exit blend.
int SessionPairAction(const RE::hkbContext& context) {
    PairedSession::Ticket ticket;
    RE::ActorHandle participant;
    if (!PairedSession::TryGetParticipant(context, participant, ticket)) return -1;
    const auto status = PairedSession::ReadStatus();
    const int action = status.action;
    if (!(status.ticket == ticket) || action < 0 || action >= Actions::Count()) return -1;
    const auto& spec = Actions::Spec(action);
    return spec.paired.enabled && spec.exitBlend >= 0 ? action : -1;
}
// The pair whose survivor this actor is, through the shared partner-loss exit, when that pair authored a loss blend
// (0.12.286): an ending the pair intends (a throw that ragdolls its partner) hands movement back as its exit does.
int SessionPairLossAction(const RE::hkbContext& context) {
    PairedSession::Ticket ticket;
    RE::ActorHandle participant;
    if (!PairedSession::TryGetParticipant(context, participant, ticket)) return -1;
    const auto status = PairedSession::ReadStatus();
    const int action = status.action;
    if (!(status.ticket == ticket) || action < 0 || action >= Actions::Count()) return -1;
    const auto& spec = Actions::Spec(action);
    return spec.paired.enabled && spec.paired.lossBlend > 0.0f ? action : -1;
}
void AddDestinationEvents(int action) {
    const auto& spec = Actions::Spec(action);
    destinationEvents.insert_or_assign(spec.events.exit, action);
    if (spec.HasRecovery()) destinationEvents.insert_or_assign(spec.recovery.recoverEvent, action);
    if (spec.paired.lossBlend > 0.0f && !spec.paired.lossExitEvent.empty())
        destinationEvents.insert_or_assign(spec.paired.lossExitEvent, action);
}
auto Key(RE::hkbNode* node, const RE::hkbContext& context) {
    return ExitBlendMailbox<Request>::Key{
        reinterpret_cast<std::uintptr_t>(node), reinterpret_cast<std::uintptr_t>(context.character)};
}

struct LocomotionActivate {
    static void thunk(RE::hkbStateMachine* node, const RE::hkbContext& context) {
        if (!node || !node->name.c_str() || std::strcmp(node->name.c_str(), "MT_Default_Behavior") != 0) {
            func(node, context);
            return;
        }
        DestinationOwner owner;
        RE::NiPointer<RE::NiAVObject> scene;
        {
            std::lock_guard lock(mutex);
            if (destination.remaining > 0) {
                owner = destination.owner;
                scene = destinationScene;
            }
        }
        auto* actor = owner.actor ? ClipHooks::ActorForContext(context) : nullptr;
        const bool owned = actor && actor->GetHandle() == owner.actor && Eligible(actor) &&
                           scene.get() == actor->Get3D(false) && ValidStatus(PairedSession::ReadStatus(), owner) &&
                           movement.load(std::memory_order_relaxed) &&
                           InputSink::Get()->CurrentMoveDir(MoveDir::kNone) != MoveDir::kNone;
        if (!owned || !node->states._data || node->states._size != 2) {
            func(node, context);
            return;
        }
        std::array<LocomotionStartState, 2> states{};
        for (int i = 0; i < 2; ++i) {
            const auto* state = reinterpret_cast<const Havok::StateInfoLayout*>(node->states._data[i]);
            if (!state || !state->name.c_str() || !state->generator || !state->enable) {
                func(node, context);
                return;
            }
            states[i] = {state->stateId, state->name.c_str()};
        }
        const auto start = PairLocomotionStart(owned, node->cloneState == RE::hkbNode::CloneState::kClone,
            node->startStateMode == RE::hkbStateMachine::StartStateMode::kDefault, node->name.c_str(),
            node->startStateID, states);
        if (!start) {
            func(node, context);
            return;
        }
        bool consumed = false;
        {
            std::lock_guard lock(mutex);
            consumed = destination.Consume(owner, reinterpret_cast<std::uintptr_t>(context.character));
        }
        if (!consumed) {
            func(node, context);
            return;
        }
        const int originalStart = node->startStateID;
        ActivateLocomotionStart(node->startStateID, *start, [&] { func(node, context); });
        logger::debug(
            "[PairBlendDestination] #{} machine='{}' default={} selected={} actual={} restored={}; native locomotion destination",
            owner.ticket.request, node->name.c_str(), originalStart, *start, node->currentStateID, node->startStateID);
    }
    static inline REL::Relocation<decltype(thunk)> func;
};
struct Activate {
    static void thunk(RE::hkbNode* node, const RE::hkbContext& context) {
        int action = -1;
        if (node && node->name.c_str()) {
            {
                std::shared_lock lock(exitsLock);
                if (const auto found = exits.find(std::string_view(node->name.c_str())); found != exits.end())
                    action = found->second;
            }
            if (mailboxItems.load(std::memory_order_acquire)) {
                std::lock_guard lock(mutex);
                DeactivateLocked(Key(node, context));
            }
        }
        if (action == kSessionPair)
            action = SessionPairAction(context);
        else if (action == kSessionPairLoss)
            action = SessionPairLossAction(context);
        if (action >= 0 && action < Actions::Count()) {
            // Keep the last pair pose before the first blend step.
            Carry::ObserveExitBlend(context);
            auto* actor = ClipHooks::ActorForContext(context);
            if (actor && actor->IsPlayerRef() && actor->Is3DLoaded()) {
                Request request;
                request.actor = actor->GetHandle();
                request.action = action;
                request.paired = Actions::Spec(action).paired.enabled;
                RE::ActorHandle participant;
                const bool valid = request.paired
                                       ? PairedSession::TryGetParticipant(context, participant, request.pair) &&
                                             participant == request.actor
                                       : true;
                if (valid) {
                    if (!request.paired) request.serial = Actions::Runner(action).PlaybackSerial();
                    std::lock_guard lock(mutex);
                    mailbox.Activate(Key(node, context), request);
                    mailboxItems.store(mailbox.Active(), std::memory_order_release);
                }
            }
        }
        func(node, context);
    }
    static inline REL::Relocation<decltype(thunk)> func;
};
struct Update {
    static void thunk(RE::hkbNode* node, const RE::hkbContext& context, float delta) {
        if (movement.load(std::memory_order_relaxed)) {
            std::lock_guard lock(mutex);
            delta = mailbox.Observe(Key(node, context), delta, true);
        }
        // Input never changes the effect clock. Mutations run outside graph evaluation.
        func(node, context, delta);
    }
    static inline REL::Relocation<decltype(thunk)> func;
};
struct Deactivate {
    static void thunk(RE::hkbNode* node, const RE::hkbContext& context) {
        if (mailboxItems.load(std::memory_order_acquire)) {
            std::lock_guard lock(mutex);
            DeactivateLocked(Key(node, context));
        }
        func(node, context);
    }
    static inline REL::Relocation<decltype(thunk)> func;
};

}
void CaptureDestination(RE::hkbStateMachine* root, const RE::hkbContext& context, const RE::hkbEvent& event) {
    if (!root || !root->name.c_str() || std::strcmp(root->name.c_str(), "RootBehaviorGraph") != 0 ||
        !context.behavior || !movement.load(std::memory_order_relaxed))
        return;
    const char* name = NodeFactory::EventNameOf(context.behavior, static_cast<int>(event.id));
    if (!name) return;
    int action = -1;
    {
        std::lock_guard lock(mutex);
        const auto found = destinationEvents.find(std::string_view(name));
        if (found != destinationEvents.end()) action = found->second;
    }
    // Native exit effects may not have a registered custom blend.
    // Match paired declarations without changing their authored transition.
    if (action < 0) {
        // Every root event reaches this point while the player moves: look the name up instead of
        // comparing it with every declaration. First pair (in action order) whose exit or recovery
        // event it is, as the former walk found.
        struct Hash {
            using is_transparent = void;
            std::size_t operator()(std::string_view text) const noexcept { return std::hash<std::string_view>{}(text); }
        };
        static const auto pairedExits = [] {
            std::unordered_map<std::string, int, Hash, std::equal_to<>> map;
            for (int i = 0; i < Actions::Count(); ++i) {
                const auto& spec = Actions::Spec(i);
                if (!spec.paired.enabled) continue;
                map.try_emplace(spec.events.exit, i);
                if (spec.HasRecovery()) map.try_emplace(spec.recovery.recoverEvent, i);
            }
            return map;
        }();
        if (const auto found = pairedExits.find(std::string_view(name)); found != pairedExits.end())
            action = found->second;
    }
    if (action < 0 || action >= Actions::Count()) return;
    auto* actor = ClipHooks::ActorForContext(context);
    if (!Actions::Spec(action).paired.enabled || !Eligible(actor) ||
        InputSink::Get()->CurrentMoveDir(MoveDir::kNone) == MoveDir::kNone)
        return;
    PairedSession::Ticket ticket;
    RE::ActorHandle participant;
    if (!PairedSession::TryGetParticipant(context, participant, ticket) || participant != actor->GetHandle()) return;
    if (!GraphInjector::IsActionState(action, "RootBehaviorGraph", root->currentStateID)) return;
    if (!ValidStatus(PairedSession::ReadStatus(), {actor->GetHandle(), ticket})) return;
    {
        std::lock_guard lock(mutex);
        destination.Arm({actor->GetHandle(), ticket});
        destinationScene.reset(actor->Get3D(false));
    }
}
void Register(RE::hkReferencedObject* effect, float duration, int action) {
    if (!effect || !std::isfinite(duration) || duration <= 0 || action < 0) return;
    auto* node = static_cast<RE::hkbNode*>(effect);
    if (!node->name.c_str()) return;
    {
        std::unique_lock lock(exitsLock);
        exits.insert_or_assign(node->name.c_str(), action);
    }
    std::lock_guard lock(mutex);
    if (action < Actions::Count() && Actions::Spec(action).paired.enabled) AddDestinationEvents(action);
}
void RegisterSharedPairExit(RE::hkReferencedObject* effect) {
    auto* node = static_cast<RE::hkbNode*>(effect);
    if (!node || !node->name.c_str()) return;
    std::unique_lock lock(exitsLock);
    exits.insert_or_assign(node->name.c_str(), kSessionPair);
}
void RegisterSharedPairLoss(RE::hkReferencedObject* effect) {
    auto* node = static_cast<RE::hkbNode*>(effect);
    if (!node || !node->name.c_str()) return;
    std::unique_lock lock(exitsLock);
    exits.insert_or_assign(node->name.c_str(), kSessionPairLoss);
}
void RegisterPairDestination(int action) {
    if (action < 0 || action >= Actions::Count() || !Actions::Spec(action).paired.enabled) return;
    std::lock_guard lock(mutex);
    AddDestinationEvents(action);
}
void Drain() {
    std::array<Request, 64> requests{};
    std::size_t count;
    {
        std::lock_guard lock(mutex);
        count = mailbox.Take(requests);
    }
    if (!movement.load(std::memory_order_relaxed)) return;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& request = requests[i];
        auto actor = request.actor.get();
        if (!actor || !actor->IsPlayerRef() || !actor->Is3DLoaded() || actor->IsDead() || actor->IsInRagdollState() ||
            request.action < 0 || request.action >= Actions::Count())
            continue;
        if (request.paired)
            PairedSession::BeginNormalExitMovement(request.pair, request.actor, request.action);
        else
            Actions::Runner(request.action).BeginNormalExitMovement(request.serial);
    }
}
void Reset() {
    movement.store(false, std::memory_order_relaxed);
    std::lock_guard lock(mutex);
    mailbox.Reset();
    mailboxItems.store(0, std::memory_order_release);
    destination.Reset();
    destinationScene.reset();
}
void SetMoveInput(bool held) { movement.store(held, std::memory_order_relaxed); }
void Tick(float delta) {
    DestinationOwner owner;
    {
        std::lock_guard lock(mutex);
        destination.Tick(delta);
        owner = destination.owner;
        if (!owner.actor) destinationScene.reset();
    }
    if (!owner.actor) return;
    auto actor = owner.actor.get();
    const bool valid = Eligible(actor.get()) && ValidStatus(PairedSession::ReadStatus(), owner);
    std::lock_guard lock(mutex);
    if (destination.owner == owner && (!valid || destinationScene.get() != actor->Get3D(false))) {
        destination.Reset();
        destinationScene.reset();
    }
}
void Install() {
    REL::Relocation<std::uintptr_t> machine{RE::VTABLE_hkbStateMachine[0]};
    LocomotionActivate::func = machine.write_vfunc(0x4, LocomotionActivate::thunk);
    REL::Relocation<std::uintptr_t> table{RE::VTABLE_hkbBlendingTransitionEffect[0]};
    Activate::func = table.write_vfunc(0x4, Activate::thunk);
    Update::func = table.write_vfunc(0x5, Update::thunk);
    Deactivate::func = table.write_vfunc(0x7, Deactivate::thunk);
    logger::info("[ExitBlendMove] normal exit input handoff installed; blend duration preserved");
}
}
