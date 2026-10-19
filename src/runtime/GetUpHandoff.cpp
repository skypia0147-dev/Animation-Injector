#include "PCH.h"
#include "runtime/GetUpHandoff.h"
#include "runtime/ActionRegistry.h"
#include "runtime/ActionRunner.h"
#include "runtime/GetUpHandoffPlan.h"
#include <atomic>
#include <utility>

namespace AnimationInjector::GetUpHandoff {
namespace {
namespace Plan = GetUpHandoffPlan;
struct Waiting {
    RE::ActorHandle actor;
    int action = -1;
    Plan::State state;
    bool running = false;
};
Waiting current;        // main thread
float getUpFor = 0.0f;  // how long the player's knock state has been kGetUp (main thread)
// An entry refused in the frame GetUpExit went out: the Root state may only take events from the next frame (1.0.0
// of the plugin started the action a frame after GetUpExit), so it is asked once more then (main thread).
struct Retry {
    RE::ActorHandle actor;
    int action = -1;
};
Retry retry;
// The waiting actor's handle for event callbacks from animation threads, and what they saw.
std::atomic<std::uint32_t> watched{0};
std::atomic<std::uint8_t> seen{0};
const RE::BSFixedString& Getup() {
    static const RE::BSFixedString s{"Getup"};
    return s;
}
const RE::BSFixedString& GetUpEnd() {
    static const RE::BSFixedString s{"GetUpEnd"};
    return s;
}
const RE::BSFixedString& GetUpExit() {
    static const RE::BSFixedString s{"GetUpExit"};
    return s;
}
const RE::BSFixedString& GetUpStart() {
    static const RE::BSFixedString s{"GetUpStart"};
    return s;
}
const RE::BSFixedString& GetUpBegin() {
    static const RE::BSFixedString s{"GetUpBegin"};
    return s;
}
const RE::BSFixedString& RagdollInstant() {
    static const RE::BSFixedString s{"RagdollInstant"};
    return s;
}
bool Held(RE::Actor* actor) {
    const auto* state = actor ? actor->AsActorState() : nullptr;
    if (!state) return false;
    return state->IsBleedingOut() || state->IsUnconscious() ||
           actor->GetActorRuntimeData().boolBits.all(RE::Actor::BOOL_BITS::kParalyzed);
}
Plan::Sample SampleOf(RE::Actor* actor) {
    Plan::Sample s;
    const auto* state = actor ? actor->AsActorState() : nullptr;
    if (!actor || !state || !actor->Is3DLoaded() || actor->IsDead()) return s;
    s.present = true;
    s.held = Held(actor);
    s.ragdoll = actor->IsInRagdollState();
    s.knock = static_cast<int>(state->GetKnockState());
    if (auto* controller = actor->GetCharController()) {
        s.controller = controller->GetHavokWorld() != nullptr;
        s.suspended = controller->flags.any(RE::CHARACTER_FLAGS::kFollowRagdoll, RE::CHARACTER_FLAGS::kNoSim);
        s.grounded = controller->context.currentState != RE::hkpCharacterStateType::kInAir;
    }
    return s;
}
// A get-up notification as the graph would have sent it: to the actor's own animation event sink, with the
// active graph as its source (the probe's verified delivery; NotifyAnimationGraph would be a graph request).
bool Deliver(RE::Actor* actor, const RE::BSFixedString& name) {
    RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
    if (!actor->GetAnimationGraphManager(manager) || !manager) return false;
    RE::BSTSmartPointer<RE::BShkbAnimationGraph> graph;
    {
        RE::BSSpinLockGuard lock(manager->GetRuntimeData().updateLock);
        const auto active = manager->GetRuntimeData().activeGraph;
        if (active >= manager->graphs.size() || !manager->graphs[active]) return false;
        graph = manager->graphs[active];
    }
    const RE::BSAnimationGraphEvent event{name, actor, RE::BSFixedString{}};
    static_cast<RE::BSTEventSink<RE::BSAnimationGraphEvent>*>(actor)->ProcessEvent(
        &event, graph->GetEventSource<RE::BSAnimationGraphEvent>());
    return true;
}
bool Request(RE::Actor* actor, int action) {
    if (action < 0) return false;
    auto* runner = Actions::RunnerFor(actor, action, true);
    return runner && runner->Request(-1);
}
// The action to play: a selecting action's Flow picks it now (0.12.253; at the handoff the body still holds the
// engine's matched get-up pose, which a `lying` test reads). -1 when its Flow stops.
int Destination(RE::Actor* actor, int action) {
    const int target = Actions::ResolveFlow(actor, action);
    if (target != action)
        logger::info("[GetUpHandoff] '{}' chose '{}'", Actions::Spec(action).id,
            target >= 0 ? Actions::Spec(target).id : std::string("nothing"));
    return target;
}
void Finish() {
    current = {};
    watched.store(0);
    seen.store(0);
}
}
bool Start(RE::Actor* actor, int action) {
    if (!actor || action < 0 || action >= Actions::Count() || !actor->Is3DLoaded() || actor->IsDead()) return false;
    const auto& spec = Actions::Spec(action);
    if (spec.paired.enabled) return false;
    const auto* state = actor->AsActorState();
    if (!state) return false;
    const int knock = static_cast<int>(state->GetKnockState());
    const bool ragdoll = actor->IsInRagdollState();
    if (!Plan::Down(knock, ragdoll)) {
        const bool accepted = Request(actor, Destination(actor, action));
        logger::info("[GetUpHandoff] action='{}' started at once accepted={}", spec.id, accepted);
        return accepted;
    }
    if (!actor->IsPlayerRef() || Held(actor)) {
        logger::info("[GetUpHandoff] action='{}' not started: the actor is held down or not the player", spec.id);
        return false;
    }
    if (Plan::TooLate(knock, ragdoll, getUpFor)) {
        logger::info(
            "[GetUpHandoff] action='{}' not started: the engine's own get-up has played {:.2f}s", spec.id, getUpFor);
        return false;
    }
    if (current.running)
        logger::info("[GetUpHandoff] action='{}' replaces '{}'", spec.id, Actions::Spec(current.action).id);
    const auto handle = actor->GetHandle();
    current = {handle, action, {}, true};
    seen.store(0);
    watched.store(handle.native_handle());
    logger::info(
        "[GetUpHandoff] action='{}' waits for the engine's get-up (knock={} ragdoll={})", spec.id, knock, ragdoll);
    return true;
}
void Update(float delta) {
    if (auto* player = RE::PlayerCharacter::GetSingleton()) {
        const auto* state = player->AsActorState();
        getUpFor = state && state->GetKnockState() == RE::KNOCK_STATE_ENUM::kGetUp ? getUpFor + delta : 0.0f;
    }
    if (retry.action >= 0) {
        const auto again = std::exchange(retry, {});
        if (auto actor = again.actor.get()) {
            const bool accepted = Request(actor.get(), again.action);
            logger::info("[GetUpHandoff] action='{}' asked again a frame after GetUpExit: accepted={}",
                Actions::Spec(again.action).id, accepted);
        }
    }
    if (!current.running) return;
    auto actor = current.actor.get();
    const auto sample = SampleOf(actor.get());
    const bool waitGround = Actions::Spec(current.action).condition.inAir == 0;
    const auto act = Plan::Advance(current.state, sample, seen.load(), waitGround, delta);
    switch (act) {
    case Plan::Act::kNone: return;
    case Plan::Act::kAlign: {
        // The engine's pull begins only so that it moves the actor onto the body (GetUpHandoffPlan.h).
        const bool sent = actor->NotifyAnimationGraph(GetUpBegin());
        logger::info("[GetUpHandoff] aligning to the body at rest t={:.3f}: GetUpBegin={}", current.state.life, sent);
        return;
    }
    case Plan::Act::kKick: {
        // The get-up begins now, without the engine's pull (GetUpHandoffPlan.h): the graph's FullyRagdoll takes
        // GetUpStart unconditionally, and the actor's sink hands the body back on it.
        const bool graph = actor->NotifyAnimationGraph(GetUpStart());
        const bool actorSink = Deliver(actor.get(), GetUpStart());
        logger::info("[GetUpHandoff] kicked the get-up at rest t={:.3f}: graph={} actor={}", current.state.life, graph,
            actorSink);
        return;
    }
    case Plan::Act::kRescueRagdoll:
        logger::warn("[GetUpHandoff] not handed back {:.2f}s after the kick (knock={} ragdoll={} controller={} "
                     "suspended={}); back to the engine's get-up",
            Plan::kKickSeconds, sample.knock, sample.ragdoll, sample.controller, sample.suspended);
        actor->NotifyAnimationGraph(RagdollInstant());
        return;
    case Plan::Act::kRescueGetUp: {
        const bool sent = actor->NotifyAnimationGraph(GetUpBegin());
        logger::info(
            "[GetUpHandoff] rescue GetUpBegin={}; action='{}' not started", sent, Actions::Spec(current.action).id);
        Finish();
        return;
    }
    case Plan::Act::kDeliverGetup:
    case Plan::Act::kDeliverGetUpEnd: {
        const auto& name = act == Plan::Act::kDeliverGetup ? Getup() : GetUpEnd();
        if (!Deliver(actor.get(), name)) {
            logger::warn("[GetUpHandoff] '{}' could not be delivered; the native get-up continues", name.c_str());
            Finish();
            return;
        }
        logger::debug("[GetUpHandoff] supplied '{}' t={:.3f}", name.c_str(), current.state.life);
        return;
    }
    case Plan::Act::kStart: {
        const int action = current.action;
        const float life = current.state.life, airborne = current.state.airborne;
        const bool leave = Plan::LeaveGetUp(seen.load());
        Finish();
        // The destination while the graph still plays the engine's clip; then out of the get-up state and into
        // the action in one frame (GetUpHandoffPlan.h): refused, the player still stands up through GetUpExit
        // instead of walking while the engine's clip lies on the ground.
        const int target = Destination(actor.get(), action);
        const bool exited = leave && actor->NotifyAnimationGraph(GetUpExit());
        const bool accepted = Request(actor.get(), target);
        logger::info("[GetUpHandoff] handed back after {:.3f}s (ground wait {:.3f}s, grounded={}); GetUpExit={} "
                     "action='{}' accepted={}",
            life, airborne, sample.grounded, exited, target >= 0 ? Actions::Spec(target).id : std::string("none"),
            accepted);
        if (!accepted && exited && target >= 0) retry = {actor->GetHandle(), target};
        return;
    }
    case Plan::Act::kGiveUp:
        logger::info("[GetUpHandoff] stopped: present={} held={} ragdoll={} knock={} controller={} suspended={} "
                     "t={:.3f} (the native get-up continues)",
            sample.present, sample.held, sample.ragdoll, sample.knock, sample.controller, sample.suspended,
            current.state.life);
        Finish();
        return;
    }
}
void ObserveEvent(RE::Actor* actor, const RE::BSFixedString& tag) {
    const auto id = watched.load();
    if (!id || !actor || actor->GetHandle().native_handle() != id) return;
    if (tag == Getup())
        seen.fetch_or(Plan::kSawGetup);
    else if (tag == GetUpEnd())
        seen.fetch_or(Plan::kSawGetUpEnd);
    else if (tag == GetUpExit())
        seen.fetch_or(Plan::kSawGetUpExit);
}
void ResetOnLoad() {
    Finish();
    retry = {};
    getUpFor = 0.0f;
}
}
