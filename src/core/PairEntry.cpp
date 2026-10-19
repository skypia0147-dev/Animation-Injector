#include "PCH.h"
#include "core/PairEntry.h"
#include <shared_mutex>

namespace AnimationInjector::PairEntry {
namespace {
struct Entry {
    RE::BSFixedString name;  // keeps the interned key alive
    Role role = Role::kLead;
    std::int32_t stateId = 0;
    Blends blends;
};
// Keyed by the interned text: BSFixedString equality is identity of the interned string.
std::unordered_map<const char*, Entry> entries;
std::shared_mutex lock;
}

void Register(const char* enterEvent, Role role, std::int32_t stateId, const Blends& blends) {
    if (!enterEvent || !*enterEvent) return;
    RE::BSFixedString name{enterEvent};
    std::unique_lock guard(lock);
    const char* key = name.c_str();
    if (const auto found = entries.find(key);
        found != entries.end() && (found->second.role != role || found->second.stateId != stateId))
        // Every character graph of the project is built from the same declarations and templates,
        // so a second graph registers the same entry; a difference means the graphs differ.
        logger::warn("[PairEntry] '{}' registered again with role {} state {} (was role {} state {}); the last wins",
            enterEvent, static_cast<int>(role), stateId, static_cast<int>(found->second.role), found->second.stateId);
    entries[key] = {std::move(name), role, stateId, blends};
}

std::size_t Count() {
    std::shared_lock guard(lock);
    return entries.size();
}

Sent BeforeNotify(RE::Actor* actor, const RE::BSFixedString& event) {
    Sent sent;
    if (!actor || event.empty()) return sent;
    Entry entry;
    {
        std::shared_lock guard(lock);
        const auto found = entries.find(event.c_str());
        if (found == entries.end()) return sent;
        entry.role = found->second.role;
        entry.stateId = found->second.stateId;
        entry.blends = found->second.blends;
    }
    static const RE::BSFixedString stateVariable{kStateVariable}, exitVariable{kExitBlendVariable},
        lossVariable{kLossBlendVariable}, recoveryVariable{kRecoveryBlendVariable}, lead{kEnterLeadEvent},
        support{kEnterSupportEvent};
    const bool blendsSet = actor->SetGraphVariableFloat(exitVariable, entry.blends.exit) &&
                           actor->SetGraphVariableFloat(lossVariable, entry.blends.loss) &&
                           actor->SetGraphVariableFloat(recoveryVariable, entry.blends.recovery);
    const bool stateSet = actor->SetGraphVariableInt(stateVariable, entry.stateId);
    sent.sent = true;
    // Re-enters the hook with an event that is not a registered entry.
    sent.accepted = actor->NotifyAnimationGraph(entry.role == Role::kLead ? lead : support);
    if (!stateSet || !blendsSet) {
        // A project without the containers (a paired target of another rig): the original event enters there.
        static std::atomic_bool reported{false};
        if (!reported.exchange(true))
            logger::info("[PairEntry] actor={:08X} graph without the shared pair variables (state={} blends={}); "
                         "the original event enters as before",
                actor->GetFormID(), stateSet, blendsSet);
    }
    return sent;
}
}
