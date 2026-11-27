#include "PCH.h"
#include "runtime/NpcDecision.h"
#include "runtime/ActionRegistry.h"
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace AnimationInjector::NpcDecision {
namespace {
std::vector<int> g_starts;  // the combat starts' actions, in declaration order
std::once_flag g_built;
const std::vector<int>& Starts() {
    std::call_once(g_built, [] {
        for (int a = 0; a < Actions::Count(); ++a)
            if (Actions::Spec(a).trigger.onCombat) g_starts.push_back(a);
        if (!g_starts.empty()) logger::info("[NpcChoice] {} combat starts", g_starts.size());
    });
    return g_starts;
}
// Per NPC: the seconds each combat start has waited while the NPC could start one. Main thread.
struct Clock {
    std::vector<float> waited;
    bool seen = false;
};
std::unordered_map<RE::FormID, Clock> g_clocks;

// The NPC could start an attack of ours now: in combat with a living, loaded target in its cell, weapon drawn, not
// attacking, on its feet and playing none of our full-body actions.
bool Ready(RE::Actor* npc, RE::Actor* target) {
    if (npc->IsDead() || !npc->Is3DLoaded() || !npc->IsInCombat() || npc->IsInRagdollState() || npc->IsInKillMove())
        return false;
    if (!target || target == npc || target->IsDead() || !target->Is3DLoaded() || !npc->GetParentCell() ||
        npc->GetParentCell() != target->GetParentCell())
        return false;
    const auto* state = npc->AsActorState();
    if (!state || !state->IsWeaponDrawn() || state->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone ||
        state->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal)
        return false;
    return !Actions::Busy(npc);
}
}

void Update(float delta) {
    const auto& starts = Starts();
    if (starts.empty() || !std::isfinite(delta) || delta <= 0.0f) return;
    auto* lists = RE::ProcessLists::GetSingleton();
    if (!lists) return;
    for (auto& [id, clock] : g_clocks) clock.seen = false;
    for (const auto& handle : lists->highActorHandles) {
        const auto ptr = handle.get();
        auto* npc = ptr.get();
        if (!npc || npc->IsPlayerRef()) continue;
        auto& clock = g_clocks[npc->GetFormID()];
        clock.seen = true;
        if (clock.waited.size() != starts.size()) clock.waited.assign(starts.size(), 0.0f);
        const RE::ActorHandle target = npc->GetActorRuntimeData().currentCombatTarget;
        const auto other = target.get();
        // Asked only after `every` seconds of being ready: an NPC that just attacked or moved in waits that long.
        if (!Ready(npc, other.get())) {
            std::fill(clock.waited.begin(), clock.waited.end(), 0.0f);
            continue;
        }
        for (std::size_t i = 0; i < starts.size(); ++i) {
            clock.waited[i] += delta;
            if (clock.waited[i] < Actions::Spec(starts[i]).trigger.every) continue;
            clock.waited[i] = 0.0f;
            // The first start whose conditions hold goes, read once here (a chance is rolled once): its destination is
            // queued and starts on the next drain.
            ConditionValues::Request request;
            const int selected = Actions::SelectStart(npc, starts[i], target, &request);
            logger::debug("[NpcChoice] actor={:08X} combat start '{}' -> {}", npc->GetFormID(),
                Actions::Spec(starts[i]).id, selected >= 0 ? Actions::Spec(selected).id : std::string("none"));
            if (selected < 0) continue;
            Actions::QueueStart(npc, selected, target);
            std::fill(clock.waited.begin(), clock.waited.end(), 0.0f);
            break;
        }
    }
    std::erase_if(g_clocks, [](const auto& entry) { return !entry.second.seen; });
}

void Reset() { g_clocks.clear(); }
}
