#pragma once
#include "PCH.h"
#include "declarations/ConditionFlowXml.h"

namespace AnimationInjector {
inline bool MatchesEquippedType(RE::TESForm* form, ConditionFlowXml::EquippedType type) {
    using T = ConditionFlowXml::EquippedType;
    if (!form) return type == T::kUnarmed;
    if (type == T::kShield) {
        auto* armor = form->As<RE::TESObjectARMO>();
        return armor && armor->IsShield();
    }
    if (type == T::kSpell) return form->As<RE::SpellItem>() != nullptr;
    if (type == T::kTorch) {
        auto* light = form->As<RE::TESObjectLIGH>();
        return light && light->CanBeCarried();
    }
    auto* weapon = form->As<RE::TESObjectWEAP>();
    if (!weapon) return false;
    using W = RE::WEAPON_TYPE;
    const auto w = weapon->GetWeaponType();
    switch (type) {
    case T::kBow: return w == W::kBow;
    case T::kCrossbow: return w == W::kCrossbow;
    case T::kSword: return w == W::kOneHandSword;
    case T::kDagger: return w == W::kOneHandDagger;
    case T::kAxe: return w == W::kOneHandAxe;
    case T::kMace: return w == W::kOneHandMace;
    case T::kGreatsword: return w == W::kTwoHandSword;
    case T::kBattleaxe: return w == W::kTwoHandAxe && !weapon->HasKeywordString("WeapTypeWarhammer");
    case T::kWarhammer: return w == W::kTwoHandAxe && weapon->HasKeywordString("WeapTypeWarhammer");
    case T::kStaff: return w == W::kStaff;
    case T::kUnarmed: return w == W::kHandToHandMelee;
    default: return false;
    }
}
inline RE::ATTACK_STATE_ENUM NativeAttackState(ConditionFlowXml::AttackState state) {
    using A = RE::ATTACK_STATE_ENUM;
    static constexpr A states[] = {A::kNone, A::kDraw, A::kSwing, A::kHit, A::kNextAttack, A::kFollowThrough, A::kBash,
        A::kBowDraw, A::kBowAttached, A::kBowDrawn, A::kBowReleasing, A::kBowReleased, A::kBowNextAttack,
        A::kBowFollowThrough};
    return states[static_cast<unsigned>(state)];
}
}
