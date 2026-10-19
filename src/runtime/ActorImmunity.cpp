#include "runtime/ActorImmunity.h"
namespace AnimationInjector::ActorImmunity {
namespace {
std::mutex mutex;
Ledger ledger;
std::vector<SavedLease> loaded;      // the co-save's leases, until PostLoad
std::vector<SavedBase> loadedBases;  // 0.12.229 co-save leases, until PostLoad
// The engine's per-actor ghost setter (the state Papyrus Actor.SetGhost sets and Actor::IsGhost reads).
// Its ids follow Actor::IsGhost (36286, 37275) in CommonLib; PayloadInterpreter's @SETGHOST, the
// usual animation i-frame, calls it with this signature.
void SetGhost(RE::Actor* actor, bool ghost) {
    using func_t = void(RE::Actor*, bool);
    static REL::Relocation<func_t> func{RELOCATION_ID(36287, 37276)};
    func(actor, ghost);
}
void TurnBack(RE::FormID id) {
    if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(id)) {
        SetGhost(actor, false);
        logger::debug("[Immunity] {:08X} ghost off (IsGhost={})", id, actor->IsGhost());
    }
}
// A 0.12.229 lease set the base's Ghost and Invulnerable flags: a flag the base did not have is cleared.
void RestoreBase(const SavedBase& saved) {
    using Flag = RE::ACTOR_BASE_DATA::Flag;
    if (auto* base = RE::TESForm::LookupByID<RE::TESNPC>(saved.base)) {
        if (!saved.ghost) base->actorData.actorBaseFlags.reset(Flag::kIsGhost);
        if (!saved.invulnerable) base->actorData.actorBaseFlags.reset(Flag::kInvulnerable);
    }
}
}
RE::FormID Acquire(RE::Actor* actor) {
    if (!actor) return 0;
    const auto id = actor->GetFormID();
    std::lock_guard lock(mutex);
    const bool held = ledger.Holders(id) > 0, wasGhost = actor->IsGhost();
    if (ledger.Acquire(id, wasGhost)) {
        SetGhost(actor, true);
        logger::debug("[Immunity] {:08X} ghost on (IsGhost={})", id, actor->IsGhost());
    } else if (held)
        logger::debug("[Immunity] {:08X} another lease ({} held)", id, ledger.Holders(id));
    else
        logger::debug("[Immunity] {:08X} was a ghost before its first lease; left alone", id);
    return id;
}
void Release(RE::FormID& lease) {
    if (!lease) return;
    std::lock_guard lock(mutex);
    if (ledger.Release(lease)) TurnBack(lease);
    lease = 0;
}
std::vector<SavedLease> Snapshot() {
    std::lock_guard lock(mutex);
    return ledger.Snapshot();
}
void SetLoaded(std::vector<SavedLease> saved) {
    std::lock_guard lock(mutex);
    loaded = std::move(saved);
}
void SetLoadedBases(std::vector<SavedBase> saved) {
    std::lock_guard lock(mutex);
    loadedBases = std::move(saved);
}
std::size_t PostLoad() {
    std::lock_guard lock(mutex);
    const auto back = ledger.Restore(loaded);
    for (const auto id : back) TurnBack(id);
    for (const auto& base : loadedBases) RestoreBase(base);
    const auto restored = back.size() + loadedBases.size();
    loaded.clear();
    loadedBases.clear();
    return restored;
}
std::size_t ResetOnLoad() {
    std::lock_guard lock(mutex);
    const auto left = ledger.Size();
    for (const auto id : ledger.Clear()) TurnBack(id);
    loaded.clear();
    loadedBases.clear();
    return left;
}
}
