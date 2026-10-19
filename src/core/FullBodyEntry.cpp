#include "PCH.h"
#include "core/FullBodyEntry.h"
#include <shared_mutex>

namespace AnimationInjector::FullBodyEntry {
namespace {
struct Entry {
    RE::BSFixedString name;  // keeps the interned key alive
    std::int32_t stateId = 0;
    float blend = 0;
    bool link = false;
};
// Keyed by the interned text: BSFixedString equality is identity of the interned string.
std::unordered_map<const char*, Entry> entries;
std::shared_mutex lock;
}

void Register(const char* enterEvent, std::int32_t stateId, float blend, bool link) {
    if (!enterEvent || !*enterEvent) return;
    RE::BSFixedString name{enterEvent};
    std::unique_lock guard(lock);
    const char* key = name.c_str();
    entries[key] = {std::move(name), stateId, blend, link};
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
        entry.stateId = found->second.stateId;
        entry.blend = found->second.blend;
        entry.link = found->second.link;
    }
    static const RE::BSFixedString stateVariable{kStateVariable}, blendVariable{kBlendVariable}, enter{kEnterEvent},
        linkEnter{kLinkEnterEvent};
    const bool blendSet = actor->SetGraphVariableFloat(blendVariable, entry.blend);
    const bool stateSet = actor->SetGraphVariableInt(stateVariable, entry.stateId);
    sent.sent = true;
    // Re-enters the hook with an event that is not a registered entry.
    sent.accepted = actor->NotifyAnimationGraph(entry.link ? linkEnter : enter);
    if (!stateSet || !blendSet) {
        // First-person graphs have neither variable nor shared entry; the forwarded event enters there.
        static std::atomic_bool reported{false};
        if (!reported.exchange(true))
            logger::info("[FullBodyEntry] actor={:08X} graph without the shared entry variables (state={} blend={}); "
                         "the original event enters as before",
                actor->GetFormID(), stateSet, blendSet);
    }
    return sent;
}
}
