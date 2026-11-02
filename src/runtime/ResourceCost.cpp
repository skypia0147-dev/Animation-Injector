#include "PCH.h"
#include "runtime/ResourceCost.h"

namespace AnimationInjector::ResourceCost {
float Maximum(RE::Actor* actor, RE::ActorValue value) {
    auto* values = actor ? actor->AsActorValueOwner() : nullptr;
    if (!values) return 0.0f;
    const float maximum = values->GetPermanentActorValue(value) +
                          actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, value);
    return std::isfinite(maximum) ? maximum : 0.0f;
}

void Spend(RE::Actor* actor, float stamina, float magicka, float health, bool percent) {
    auto* values = actor ? actor->AsActorValueOwner() : nullptr;
    if (!values) return;
    const auto amount = [&](float written, RE::ActorValue value) {
        return percent ? OfMaximum(written, Maximum(actor, value)) : written;
    };
    constexpr auto kDamage = RE::ACTOR_VALUE_MODIFIER::kDamage;
    if (const float s = amount(stamina, RE::ActorValue::kStamina); s > 0.0f)
        values->ModActorValue(kDamage, RE::ActorValue::kStamina, -s);
    if (const float m = amount(magicka, RE::ActorValue::kMagicka); m > 0.0f)
        values->ModActorValue(kDamage, RE::ActorValue::kMagicka, -m);
    const float h = Health(amount(health, RE::ActorValue::kHealth), values->GetActorValue(RE::ActorValue::kHealth));
    if (h > 0.0f) values->ModActorValue(kDamage, RE::ActorValue::kHealth, -h);
}
}  // namespace AnimationInjector::ResourceCost
