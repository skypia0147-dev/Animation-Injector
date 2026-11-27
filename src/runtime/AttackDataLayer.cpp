#include "runtime/AttackDataLayer.h"
#include "runtime/AttackDataSlots.h"
#include <unordered_map>

namespace AnimationInjector::AttackDataLayer {
namespace {
using Data = RE::NiPointer<RE::BGSAttackData>;
std::unordered_map<RE::FormID, AttackDataSlots<Data>> entries;

RE::HighProcessData* HighOf(RE::Actor* actor) {
    auto* process = actor ? actor->GetActorRuntimeData().currentProcess : nullptr;
    return process ? process->high : nullptr;
}
const char* NameOf(const Data& data) { return data ? data->event.c_str() : "(none)"; }
// One change of the actor's entry: `change` gets the entry and the engine's value now and says what to write.
template<class Change>
void Apply(RE::Actor* actor, bool create, const char* what, Change&& change) {
    if (!actor) return;
    const auto id = actor->GetFormID();
    auto found = entries.find(id);
    if (found == entries.end()) {
        if (!create) return;
        found = entries.emplace(id, AttackDataSlots<Data>{}).first;
    }
    auto* high = HighOf(actor);
    if (!high) {
        // No high process to write to (the actor left it): what we wrote went with it.
        entries.erase(found);
        return;
    }
    if (const auto write = change(found->second, high->attackData)) {
        logger::debug("[AttackData] {:08X} {}: '{}' -> '{}'", id, what, NameOf(high->attackData), NameOf(*write));
        high->attackData = *write;
    }
    if (found->second.Empty()) entries.erase(found);
}
}

RE::BGSAttackData* Find(RE::Actor* actor, const std::string& name, const char* who) {
    auto* race = actor ? actor->GetRace() : nullptr;
    if (!race || !race->attackDataMap) {
        logger::warn("[AttackData] {}: no race or attack data map", who);
        return nullptr;
    }
    auto& map = race->attackDataMap->attackDataMap;
    const auto it = map.find(RE::BSFixedString(name));
    if (it != map.end() && it->second) return it->second.get();
    std::string keys;
    for (const auto& e : map) {
        if (!keys.empty()) keys += ", ";
        keys += e.first.c_str() ? e.first.c_str() : "?";
    }
    logger::warn("[AttackData] {}: race '{}' has no attack data '{}'. Names it has: {}", who, race->GetFormEditorID(),
        name, keys);
    return nullptr;
}

void SetAction(RE::Actor* actor, int owner, RE::BGSAttackData* data) {
    if (!data) return;
    Apply(actor, true, "action",
        [&](AttackDataSlots<Data>& slots, const Data& current) { return slots.SetAction(current, owner, Data(data)); });
}
void ClearAction(RE::Actor* actor, int owner) {
    Apply(actor, false, "action ended",
        [&](AttackDataSlots<Data>& slots, const Data& current) { return slots.ClearAction(current, owner); });
}
bool SetEvent(RE::Actor* actor, const std::string& name, const char* who) {
    if (!HighOf(actor)) return false;
    auto* data = Find(actor, name, who);
    if (!data) return false;
    Apply(actor, true, "event",
        [&](AttackDataSlots<Data>& slots, const Data& current) { return slots.SetEvent(current, Data(data)); });
    return true;
}
void ClearEvent(RE::Actor* actor) {
    Apply(actor, false, "event ended",
        [&](AttackDataSlots<Data>& slots, const Data& current) { return slots.ClearEvent(current); });
}
bool EventInEffect(RE::Actor* actor) {
    auto* high = HighOf(actor);
    const auto found = actor ? entries.find(actor->GetFormID()) : entries.end();
    return high && found != entries.end() && found->second.EventInEffect(high->attackData);
}

void Reset() { entries.clear(); }
}
