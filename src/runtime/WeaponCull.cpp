#include "PCH.h"
#include "runtime/WeaponCull.h"

namespace AnimationInjector::WeaponCull {
namespace {
Ledger<RE::NiPointer<RE::NiAVObject>> ledger;
Owner next = 0;
}
Owner NewOwner() { return ++next; }
void Hide(Owner owner, RE::Actor* actor) {
    auto biped = owner && actor && actor->Is3DLoaded() ? actor->GetActorRuntimeData().biped : nullptr;
    if (!biped) return;
    constexpr RE::BIPED_OBJECTS::BIPED_OBJECT slots[] = {RE::BIPED_OBJECTS::kShield, RE::BIPED_OBJECTS::kOneHandSword,
        RE::BIPED_OBJECTS::kOneHandDagger, RE::BIPED_OBJECTS::kOneHandAxe, RE::BIPED_OBJECTS::kOneHandMace,
        RE::BIPED_OBJECTS::kTwoHandMelee, RE::BIPED_OBJECTS::kStaff, RE::BIPED_OBJECTS::kBow,
        RE::BIPED_OBJECTS::kCrossbow};
    for (auto slot : slots) {
        auto node = biped->objects[static_cast<std::size_t>(slot)].partClone;
        if (!node) continue;
        ledger.Hide(owner, node, node->GetAppCulled());
        node->SetAppCulled(true);
    }
}
void Show(Owner owner) {
    ledger.Show(owner, [](const RE::NiPointer<RE::NiAVObject>& node, bool culled) {
        if (node) node->SetAppCulled(culled);
    });
}
std::size_t Count(Owner owner) { return ledger.Count(owner); }
void Reset() { ledger.Clear(); }
}
