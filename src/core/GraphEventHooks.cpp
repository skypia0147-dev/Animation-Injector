#include "PCH.h"
#include "core/GraphEventHooks.h"
#include "core/GraphInjector.h"
#include "runtime/ActionRegistry.h"
#include "runtime/ActionRunner.h"
#include "Settings.h"
#include "runtime/PairedSession.h"
#include "core/NodeFactory.h"
#include "core/ExitBlendHooks.h"
#include <atomic>
#include <mutex>
#include <unordered_set>

namespace AnimationInjector::GraphEventHooks {

namespace {
// Root IdleStop cannot distinguish the retiring locomotion graph from the new action.
bool ProtectHandoff(RE::hkbStateMachine* root, const RE::hkbContext& context, const RE::hkbEvent& event) {
    if (!root->name.c_str() || std::strcmp(root->name.c_str(), "RootBehaviorGraph") != 0 || !context.behavior)
        return false;
    const char* name = NodeFactory::EventNameOf(context.behavior, static_cast<int>(event.id));
    if (!name || std::strcmp(name, "IdleStop") != 0) return false;
    RE::ActorHandle handle;
    const int action = PairedSession::RetiringHandoffDestination(context, handle);
    if (action < 0 || action >= Actions::Count() || Actions::Spec(action).events.exit == "IdleStop") return false;
    auto actor = handle.get();
    if (!actor || !actor->Is3DLoaded() || actor->IsDead() || actor->IsInRagdollState() ||
        actor->AsActorState()->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal)
        return false;
    for (int i = 0; i < root->states._size; ++i) {
        const auto* state = reinterpret_cast<const Havok::StateInfoLayout*>(root->states._data[i]);
        if (!state || state->stateId != root->currentStateID || !state->name.c_str() ||
            std::strcmp(state->name.c_str(), "FullBodyPlayerIdles") != 0 ||
            !NodeFactory::SameVTable(state->generator, RE::VTABLE_hkbStateMachine))
            continue;
        // StateInfo references node templates. Their currentStateID is not the actor's clone state.
        // The retiring ticket already identifies this actor's accepted ordinary destination.
        if (!GraphInjector::HasFullBodyRoute(action)) return false;
        logger::debug("[HandoffIdleStop] actor={:08X} action='{}' root={} retiring-pair stop ignored",
            actor->GetFormID(), Actions::Spec(action).id, root->currentStateID);
        return true;
    }
    return false;
}
// Debug only. handleEvent runs on the animation worker threads that update different actors at the same
// time (code review 2026-09-30 R-15): the per-name log state is shared, so it is guarded.
std::mutex g_probeLock;
std::unordered_set<int> g_probeSeen;  // event ids logged once already
std::atomic<int> g_probeLogged{0};
// Whether this event gets a probe line: a transition or an attack event every time, another event once.
bool ProbeLine(int id, bool always) {
    std::lock_guard lock(g_probeLock);
    if (!g_probeSeen.insert(id).second && !always) return false;
    g_probeLogged.fetch_add(1, std::memory_order_relaxed);
    return true;
}
struct SmHandleEvent {
    static void thunk(RE::hkbStateMachine* a_this, const RE::hkbContext& a_ctx, const RE::hkbEvent a_ev) {
        if (ProtectHandoff(a_this, a_ctx, a_ev)) return;
        ExitBlendHooks::CaptureDestination(a_this, a_ctx, a_ev);
        if (!FrameworkSettings::Get().debug) {
            func(a_this, a_ctx, a_ev);
            return;
        }
        const char* smName = a_this->name.c_str();
        const bool ours = smName && GraphInjector::IsOurStateId(smName, a_this->currentStateID);
        PairedSession::Ticket ticket;
        RE::ActorHandle participant;
        const bool parent = smName && std::strcmp(smName, "RootBehaviorGraph") == 0;
        const bool pair = (ours || parent) && PairedSession::TryGetParticipant(a_ctx, participant, ticket);
        // Include parent exits: the child may remain unchanged while its parent fades out.
        if (parent && (pair || Actions::AnyActive())) {
            const int before = a_this->currentStateID;
            const int active = a_this->activeTransitions._size;
            const int delayed = a_this->delayedTransitions._size;
            const char* event =
                a_ctx.behavior ? NodeFactory::EventNameOf(a_ctx.behavior, static_cast<int>(a_ev.id)) : "?";
            func(a_this, a_ctx, a_ev);
            if (before != a_this->currentStateID || active != a_this->activeTransitions._size ||
                delayed != a_this->delayedTransitions._size ||
                (event && (std::strcmp(event, "IdleStop") == 0 || std::strcmp(event, "MTState") == 0))) {
                auto actor = participant.get();
                logger::debug(
                    "[ParentExitTrace] character={} actor={:08X} event='{}' state={}->{} active={}->{} delayed={}->{}",
                    reinterpret_cast<std::uintptr_t>(a_ctx.character), actor ? actor->GetFormID() : 0,
                    event ? event : "?", before, a_this->currentStateID, active, a_this->activeTransitions._size,
                    delayed, a_this->delayedTransitions._size);
            }
            return;
        }
        const bool watch =
            g_probeLogged.load(std::memory_order_relaxed) < 2000 && ours && (pair || Actions::AnyActive());
        if (!watch) {
            func(a_this, a_ctx, a_ev);
            return;
        }
        const int id = static_cast<int>(a_ev.id);
        const char* en = pair && a_ctx.behavior ? NodeFactory::EventNameOf(a_ctx.behavior, id)
                                                : GraphInjector::EventName(smName, id);
        const auto before = a_this->currentStateID;
        func(a_this, a_ctx, a_ev);
        const bool moved = before != a_this->currentStateID;
        if (pair && (moved || (en && std::strstr(en, "PartnerLossExit")))) {
            logger::debug("[PairLossBlend] #{} graph event='{}' state {} -> {} activeTransitions={} delayed={}",
                ticket.request, en ? en : "?", before, a_this->currentStateID, a_this->activeTransitions._size,
                a_this->delayedTransitions._size);
        }
        bool isAttack = false;
        if (en)
            for (const char* c = en; *c; ++c)
                if (_strnicmp(c, "attack", 6) == 0) {
                    isAttack = true;
                    break;
                }
        if (ProbeLine(id, moved || isAttack)) {
            float elapsed = 0.0f;
            // Debug only; the last busy runner of the player, as before (never creates one here).
            Actions::FindRunner(RE::PlayerCharacter::GetSingleton(), [&](int, ActionRunner& r) {
                if (r.GetState() != ActionRunner::State::kIdle) elapsed = r.Elapsed();
                return false;
            });
            logger::debug("[Probe] {} 이벤트 '{}'(id {}) at {:.2f}s: state {} → {}{} (delayed={}{} active={})", smName,
                en ? en : "?", id, elapsed, before, a_this->currentStateID, moved ? " ★전이" : "",
                a_this->delayedTransitions._size, a_this->delayedTransitions._size > 0 ? " ★선입력 보관" : "",
                a_this->activeTransitions._size);
        }
    }
    static inline REL::Relocation<decltype(thunk)> func;
};
}

void Install() {
    REL::Relocation<std::uintptr_t> vtbl{RE::VTABLE_hkbStateMachine[0]};
    SmHandleEvent::func = vtbl.write_vfunc(0x6, SmHandleEvent::thunk);
    logger::debug("[Probe] hkbStateMachine::handleEvent handoff guard installed (diagnostics optional)");
}

}  // namespace AnimationInjector::GraphEventHooks
