#include "PCH.h"
#include "runtime/Strike.h"
#include "runtime/EventEffects.h"
#include "runtime/PairFeatures.h"
#include "runtime/RagdollPush.h"
#include "RE/B/bhkPickData.h"
#include "RE/H/hkpClosestRayHitCollector.h"
#include <numbers>

namespace AnimationInjector::Strike {
namespace {
// An open window (StrikeSpec::duration): the actors it already hit are not hit again.
struct Window {
    RE::ActorHandle attacker, partner;
    StrikeSpec spec;
    float left = 0.0f;
    std::vector<RE::ActorHandle> hit;
};
std::vector<Window> windows;

bool Usable(RE::Actor* actor) { return actor && actor->Is3DLoaded() && !actor->IsDead() && !actor->IsInRagdollState(); }

// Only fixed geometry hides a target: walls, rocks, terrain and trees. Actors and loose objects do not.
class Wall final : public RE::hkpClosestRayHitCollector {
public:
    Wall() { Reset(); }
    void AddRayHit(const RE::hkpCdBody& body, const RE::hkpShapeRayCastCollectorOutput& hit) override {
        const auto* root = &body;
        unsigned depth = 0;
        while (root->parent && depth++ < 32) root = root->parent;
        if (root->parent) return;
        const auto layer = static_cast<const RE::hkpCollidable*>(root)->GetCollisionLayer();
        if (layer == RE::COL_LAYER::kStatic || layer == RE::COL_LAYER::kAnimStatic ||
            layer == RE::COL_LAYER::kTerrain || layer == RE::COL_LAYER::kGround || layer == RE::COL_LAYER::kTrees)
            RE::hkpClosestRayHitCollector::AddRayHit(body, hit);
    }
};
// A ray between the two actors' chests: false when fixed geometry stands between them. Our own ray, not
// Actor::HasLineOfSight, which for the player asks whether the target is on screen (a 360° strike hits behind).
bool Clear(RE::Actor* attacker, RE::Actor* target) {
    auto* cc = attacker->GetCharController();
    auto* world = cc ? cc->GetHavokWorld() : nullptr;
    const float scale = RE::bhkWorld::GetWorldScale();
    if (!world || !world->GetWorld1() || !std::isfinite(scale) || scale <= 0) return true;
    constexpr float kChest = 80.0f;
    const auto from = attacker->GetPosition(), to = target->GetPosition();
    RE::bhkPickData pick{};
    Wall collector;
    pick.rayInput.filterInfo.filter = 0;
    pick.rayInput.from = {from.x * scale, from.y * scale, (from.z + kChest) * scale, 0};
    pick.rayInput.to = {to.x * scale, to.y * scale, (to.z + kChest) * scale, 0};
    pick.rayOutput.Reset();
    pick.closestRayHitCollector = &collector;
    RE::BSReadLockGuard lock(world->worldLock);
    world->PickObject(pick);
    return !collector.rayHit.HasHit();
}

bool Hostile(RE::Actor* attacker, RE::Actor* target) {
    return target->IsHostileToActor(attacker) || attacker->IsHostileToActor(target);
}

// The share of the damage a target at `distance` takes (StrikeSpec::edge): 1 at the attacker, `edge` at the radius.
float Falloff(const StrikeSpec& s, float distance) {
    if (s.pick != StrikeSpec::Pick::kArea || s.edge >= 1.0f || s.radius <= 0) return 1.0f;
    const float t = std::clamp(distance / s.radius, 0.0f, 1.0f);
    return 1.0f + (s.edge - 1.0f) * t;
}

// In the area: horizontal distance, the wedge around the facing (whole width), the feet's height, no wall.
bool InArea(RE::Actor* attacker, RE::Actor* target, const StrikeSpec& s, float& distance) {
    const auto a = attacker->GetPosition(), t = target->GetPosition();
    const float dx = t.x - a.x, dy = t.y - a.y, dz = t.z - a.z;
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz)) return false;
    distance = std::sqrt(dx * dx + dy * dy);
    if (distance > s.radius || dz > s.up || dz < -s.down) return false;
    if (s.angle < 360.0f && distance > 1.0f) {
        // Skyrim's heading: 0 = +y, growing clockwise; the target's bearing the same way.
        float off = std::atan2(dx, dy) - attacker->GetAngleZ();
        while (off > std::numbers::pi_v<float>) off -= 2 * std::numbers::pi_v<float>;
        while (off < -std::numbers::pi_v<float>) off += 2 * std::numbers::pi_v<float>;
        if (std::fabs(off) * 180.0f / std::numbers::pi_v<float> > s.angle / 2) return false;
    }
    return Clear(attacker, target);
}

// The actors the strike may hit now, each with its falloff.
std::vector<std::pair<RE::Actor*, float>> Targets(RE::Actor* attacker, RE::ActorHandle partner, const StrikeSpec& s) {
    std::vector<std::pair<RE::Actor*, float>> out;
    auto add = [&](RE::Actor* target) {
        if (target && target != attacker && Usable(target)) out.emplace_back(target, 1.0f);
    };
    switch (s.pick) {
    case StrikeSpec::Pick::kPartner: add(partner.get().get()); break;
    case StrikeSpec::Pick::kCombatTarget:
        // The player has no combat target of its own: its crosshair actor stands in.
        if (!attacker->IsPlayerRef()) {
            add(attacker->GetActorRuntimeData().currentCombatTarget.get().get());
            break;
        }
        [[fallthrough]];
    case StrikeSpec::Pick::kCrosshair:
        if (attacker->IsPlayerRef())
            if (auto* pick = RE::CrosshairPickData::GetSingleton())
                if (auto ref = pick->GetActiveTarget().get()) add(ref->As<RE::Actor>());
        break;
    case StrikeSpec::Pick::kArea: {
        auto consider = [&](RE::Actor* target) {
            float distance = 0;
            if (!target || target == attacker || !Usable(target)) return;
            if (s.hostileOnly && !Hostile(attacker, target)) return;
            if (InArea(attacker, target, s, distance)) out.emplace_back(target, Falloff(s, distance));
        };
        if (auto* lists = RE::ProcessLists::GetSingleton())
            for (auto& handle : lists->highActorHandles) consider(handle.get().get());
        consider(RE::PlayerCharacter::GetSingleton());
        break;
    }
    }
    return out;
}

// A melee weapon in the hand, or none (bows, staves, spells and an empty hand strike unarmed).
RE::InventoryEntryData* MeleeEntry(RE::Actor* attacker, bool left) {
    auto* entry = attacker->GetEquippedEntryData(left);
    auto* weapon = entry && entry->object ? entry->object->As<RE::TESObjectWEAP>() : nullptr;
    if (!weapon || weapon->IsBow() || weapon->IsCrossbow() || weapon->IsStaff()) return nullptr;
    return entry;
}
// One hand's part of a strike: the engine's own hit (checked in game against the target's lastHitData, 0.12.292):
// weapon, skill, perks, the target's armour and its block, before the difficulty.
struct HandHit {
    RE::InventoryEntryData* entry = nullptr;  // the melee weapon, none unarmed
    bool left = false;
    float damage = 0.0f;
    bool blocked = false;
};
HandHit HandDamage(RE::Actor* attacker, RE::Actor* target, bool left, StrikeSpec::Block block) {
    HandHit out{MeleeEntry(attacker, left), left};
    auto* hit = RE::HitData::Create(attacker, target, out.entry, left);
    if (!hit) return out;
    float damage = hit->totalDamage;
    out.blocked = hit->percentBlocked > 0.0f;
    if (block == StrikeSpec::Block::kIgnore && hit->percentBlocked > 0.0f && hit->percentBlocked < 1.0f) {
        damage /= 1.0f - hit->percentBlocked;
        out.blocked = false;
    }
    hit->~HitData();  // releases the attackData reference Populate may have taken
    RE::free(hit);
    out.damage = std::isfinite(damage) && damage > 0 ? damage : 0.0f;
    return out;
}
std::vector<HandHit> Hands(RE::Actor* attacker, RE::Actor* target, const StrikeSpec& s) {
    std::vector<HandHit> out;
    if (s.base == StrikeSpec::Base::kFixed) {
        // A fixed amount has no weapon of its own; the right hand's still names its impact and the hit event.
        out.push_back({MeleeEntry(attacker, false), false, s.amount, false});
        return out;
    }
    if (s.hand != StrikeSpec::Hand::kLeft) out.push_back(HandDamage(attacker, target, false, s.block));
    if (s.hand != StrikeSpec::Hand::kRight) out.push_back(HandDamage(attacker, target, true, s.block));
    for (auto& h : out) h.damage *= s.ratio;
    return out;
}

RE::TESObjectWEAP* WeaponOf(const HandHit& h) {
    return h.entry && h.entry->object ? h.entry->object->As<RE::TESObjectWEAP>() : nullptr;
}
// A spell cast on the target as the attacker's own, from the hand that struck.
void Cast(RE::Actor* attacker, RE::Actor* target, RE::MagicItem* spell, bool left) {
    auto* caster = attacker->GetMagicCaster(
        left ? RE::MagicSystem::CastingSource::kLeftHand : RE::MagicSystem::CastingSource::kRightHand);
    if (caster && spell) caster->CastSpellImmediate(spell, false, target, 1.0f, false, 0.0f, attacker);
}
// The weapon's enchantment. The player's weapon spends its charge (the equipped hand's ItemCharge value) and does
// nothing when it is too low, as a swing with an empty weapon; NPCs' weapons do not run out.
void Enchantment(RE::Actor* attacker, RE::Actor* target, const HandHit& h) {
    auto* enchantment = h.entry ? h.entry->GetEnchantment() : nullptr;
    if (!enchantment) return;
    if (attacker->IsPlayerRef()) {
        auto* values = attacker->AsActorValueOwner();
        const auto charge = h.left ? RE::ActorValue::kLeftItemCharge : RE::ActorValue::kRightItemCharge;
        const float cost = enchantment->CalculateMagickaCost(attacker);
        if (!values || !(cost >= 0.0f) || values->GetActorValue(charge) < cost) return;
        values->DamageActorValue(charge, cost);
    }
    Cast(attacker, target, enchantment, h.left);
}
// The weapon's poison: cast, one dose spent, removed with the last.
void Poison(RE::Actor* attacker, RE::Actor* target, const HandHit& h) {
    if (!h.entry || !h.entry->extraLists) return;
    for (auto* list : *h.entry->extraLists) {
        auto* extra = list ? list->GetByType<RE::ExtraPoison>() : nullptr;
        if (!extra || !extra->poison || extra->count == 0) continue;
        Cast(attacker, target, extra->poison, h.left);
        if (--extra->count == 0) list->Remove(RE::ExtraDataType::kPoison, extra);
        return;
    }
}
// The attacker's "Apply Combat Hit Spell" perk entries (the entry point's conditions: weapon, target).
void Perks(RE::Actor* attacker, RE::Actor* target, const HandHit& h) {
    auto* weapon = WeaponOf(h);
    if (!weapon) return;
    RE::SpellItem* spell = nullptr;
    RE::BGSEntryPoint::HandleEntryPoint(
        RE::BGSEntryPoint::ENTRY_POINT::kApplyCombatHitSpell, attacker, weapon, target, std::addressof(spell));
    if (spell) Cast(attacker, target, spell, h.left);
}
// The material a hit lands on, as the engine looks an impact up: a blocked hit the blocker's shield or weapon
// (its block material), else the target race's blood material.
RE::BGSMaterialType* Material(RE::Actor* target, bool blocked) {
    if (blocked) {
        if (auto* shield = target->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kShield);
            shield && shield->altBlockMaterialType)
            return shield->altBlockMaterialType;
        for (const bool left : {false, true}) {
            auto* object = target->GetEquippedObject(left);
            auto* weapon = object ? object->As<RE::TESObjectWEAP>() : nullptr;
            if (weapon && weapon->altBlockMaterialType) return weapon->altBlockMaterialType;
        }
    }
    auto* race = target->GetRace();
    return race ? race->bloodImpactMaterial : nullptr;
}
// The weapon's impact (the race's unarmed set for a fist) for the material it lands on, played at the target's
// spine: the impact's sounds following that bone and its particle (the blood spray) on it. 0.12.294 called
// BGSImpactManager::PlayImpactEffect instead, which gave neither sound nor blood in game.
void Impact(RE::Actor* attacker, RE::Actor* target, const HandHit& h) {
    auto* weapon = WeaponOf(h);
    auto* race = attacker->GetRace();
    auto* set = weapon && weapon->impactDataSet ? weapon->impactDataSet : race ? race->impactDataSet : nullptr;
    auto* material = Material(target, h.blocked);
    if (!set || !material) return;
    const auto found = set->impactMap.find(material);
    auto* impact = found != set->impactMap.end() ? found->second : nullptr;
    if (!impact) {
        logger::debug("[Strike] no impact in {:08X} for material {:08X}", set->GetFormID(), material->GetFormID());
        return;
    }
    static const RE::BSFixedString kSpine("NPC Spine2 [Spn2]");
    auto* node = target->GetNodeByName(kSpine);
    if (!node) node = target->Get3D();
    if (!node) return;
    if (auto* audio = RE::BSAudioManager::GetSingleton())
        for (auto* sound : {impact->sound1, impact->sound2}) {
            RE::BSSoundHandle handle;
            if (sound && audio->GetSoundHandle(handle, sound)) {
                handle.SetPosition(node->world.translate);
                handle.SetObjectToFollow(node);
                handle.Play();
            }
        }
    const char* model = impact->GetModel();
    if (auto* cell = target->GetParentCell(); cell && model && *model)
        RE::BSTempEffectParticle::Spawn(cell, impact->data.effectDuration > 0.0f ? impact->data.effectDuration : 1.0f,
            model, node->world.rotate, node->world.translate, 1.0f, 7, node);
    logger::debug("[Strike] impact {:08X} material {:08X} sounds {:08X}/{:08X} model '{}'", impact->GetFormID(),
        material->GetFormID(), impact->sound1 ? impact->sound1->GetFormID() : 0u,
        impact->sound2 ? impact->sound2->GetFormID() : 0u, model ? model : "");
}
bool sending = false;
// A TESHitEvent as the engine sends for a hit: other mods and Papyrus OnHit hear it.
void HitEvent(RE::Actor* attacker, RE::Actor* target, const HandHit& h) {
    auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
    if (!holder) return;
    auto* weapon = WeaponOf(h);
    RE::TESHitEvent event(target, attacker, weapon ? weapon->GetFormID() : 0x1F4, 0,
        h.blocked ? RE::TESHitEvent::Flag::kHitBlocked : RE::TESHitEvent::Flag::kNone);
    sending = true;
    holder->GetEventSource<RE::TESHitEvent>()->SendEvent(std::addressof(event));
    sending = false;
}

// The vanilla stagger: magnitude, the direction the attacker stands in from the target's facing (0 in front, 0.5
// behind; the stagger blend is cyclic), then staggerStart.
void Stagger(RE::Actor* attacker, RE::Actor* target, float magnitude) {
    if (EventEffects::HyperArmorFor(target->GetHandle())) return;
    static const RE::BSFixedString kMagnitude("staggerMagnitude"), kDirection("staggerDirection"),
        kStart("staggerStart");
    const auto a = attacker->GetPosition(), t = target->GetPosition();
    constexpr float kTau = 2 * std::numbers::pi_v<float>;
    float turn = std::fmod(std::atan2(a.x - t.x, a.y - t.y) - target->GetAngleZ(), kTau);
    if (turn < 0) turn += kTau;
    target->SetGraphVariableFloat(kDirection, std::isfinite(turn) ? turn / kTau : 0.0f);
    target->SetGraphVariableFloat(kMagnitude, std::clamp(magnitude, 0.0f, 1.0f));
    target->NotifyAnimationGraph(kStart);
}
// The engine's ragdoll pushed away from the attacker (as EventOp::kRagdoll; a pair's actor first leaves its hold).
void Push(RE::Actor* attacker, RE::Actor* target, float force) {
    auto* process = target->GetActorRuntimeData().currentProcess;
    if (!process || target->IsOnMount() || !(force > 0.0f)) return;
    PairFeatures::ReleasePhysics(target->GetHandle());
    auto origin = attacker->GetPosition();
    origin.z = target->GetPosition().z + 40.0f;
    process->KnockExplosion(target, origin, std::min(force, RagdollPush::kMaxForce));
}

void Hit(RE::Actor* attacker, RE::Actor* target, float share, const StrikeSpec& s) {
    const auto hands = Hands(attacker, target, s);
    float damage = 0.0f;
    for (const auto& h : hands) damage += h.damage;
    damage *= share;
    if (!(damage > 0.0f)) return;
    auto* values = target->AsActorValueOwner();
    const float before = values ? values->GetActorValue(RE::ActorValue::kHealth) : 0.0f;
    target->DoDamage(damage, attacker, false);
    const bool alive = !target->IsDead();
    logger::info("[Strike] {:08X} -> {:08X} damage={:.2f} share={:.2f} health {:.2f} -> {:.2f}{}",
        attacker->GetFormID(), target->GetFormID(), damage, share, before,
        values ? values->GetActorValue(RE::ActorValue::kHealth) : 0.0f, alive ? "" : " (killed)");
    // What a real hit brings along: the spells only on a living target, the impact and the event always.
    const bool weapon = s.base == StrikeSpec::Base::kWeapon;
    for (const auto& h : hands) {
        if (weapon && alive && s.enchantment) Enchantment(attacker, target, h);
        if (weapon && alive && s.poison) Poison(attacker, target, h);
        if (weapon && alive && s.perks) Perks(attacker, target, h);
    }
    if (!hands.empty() && s.impact) Impact(attacker, target, hands.front());
    if (!hands.empty() && s.hitEvent) HitEvent(attacker, target, hands.front());
    if (!alive || !Usable(target)) return;
    if (s.reaction == StrikeSpec::Reaction::kStagger) Stagger(attacker, target, s.stagger * share);
    if (s.reaction == StrikeSpec::Reaction::kPush) Push(attacker, target, s.force * share);
}

// Hits every target now in reach that the window has not hit yet (all of them without a window).
void Pass(RE::Actor* attacker, RE::ActorHandle partner, const StrikeSpec& s, std::vector<RE::ActorHandle>* already) {
    for (const auto& [target, share] : Targets(attacker, partner, s)) {
        const auto handle = target->GetHandle();
        if (already) {
            if (std::find(already->begin(), already->end(), handle) != already->end()) continue;
            already->push_back(handle);
        }
        Hit(attacker, target, share, s);
    }
}
}  // namespace

void Run(RE::Actor* attacker, RE::ActorHandle partner, const StrikeSpec& spec) {
    if (!Usable(attacker)) return;
    if (spec.duration <= 0.0f) {
        Pass(attacker, partner, spec, nullptr);
        return;
    }
    Window w{attacker->GetHandle(), partner, spec, spec.duration, {}};
    Pass(attacker, partner, spec, &w.hit);
    windows.push_back(std::move(w));
}

void Update(float delta) {
    if (windows.empty()) return;
    if (!std::isfinite(delta) || delta < 0 || delta > 1.0f) delta = 0.0f;
    for (auto it = windows.begin(); it != windows.end();) {
        auto attacker = it->attacker.get();
        it->left -= delta;
        if (!Usable(attacker.get()) || it->left <= 0.0f) {
            it = windows.erase(it);
            continue;
        }
        Pass(attacker.get(), it->partner, it->spec, &it->hit);
        ++it;
    }
}

void ResetOnLoad() { windows.clear(); }

bool Sending() { return sending; }

}  // namespace AnimationInjector::Strike
