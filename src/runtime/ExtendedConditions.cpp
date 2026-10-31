#include "PCH.h"
#include "runtime/ExtendedConditions.h"
#include "runtime/ActionHistory.h"
#include "runtime/HitHistory.h"
#include "runtime/LyingSide.h"
#include "runtime/PairedRig.h"
#include "runtime/PairedSession.h"
#include <algorithm>
#include <numbers>

namespace AnimationInjector::ExtendedConditions {
ConditionFlow::Value Evaluate(const ConditionFlowXml::Predicate& p, RE::Actor* subject, RE::Actor* counterpart,
    int flow, int index, ConditionValues::Request& request) {
    using Test = ConditionFlowXml::Test;
    using Value = ConditionFlow::Value;
    const auto boolean = [](bool value) { return value ? Value::kTrue : Value::kFalse; };
    const auto numeric = [&](double value) {
        return std::isfinite(value) ? boolean(ConditionValues::Compare(value, p.compare, p.value, p.upper))
                                    : Value::kUnavailable;
    };
    if (!subject || !subject->Is3DLoaded()) return Value::kUnavailable;
    RE::TESForm* form = nullptr;
    if (!p.plugin.empty()) {
        auto* data = RE::TESDataHandler::GetSingleton();
        form = data ? data->LookupForm(p.form, p.plugin) : nullptr;
        if (!form) return Value::kUnavailable;
    } else if (!p.keyword.empty()) {
        // Named by editor ID: the keyword ActionRegistry found once the game data was in.
        form = p.keywordForm ? RE::TESForm::LookupByID(p.keywordForm) : nullptr;
        if (!form) return Value::kUnavailable;
    }
    const auto pairReady = [&] {
        return counterpart && counterpart != subject && counterpart->Is3DLoaded() && subject->GetParentCell() &&
               subject->GetParentCell() == counterpart->GetParentCell();
    };
    switch (p.test) {
    case Test::kLying: {
        // The body's side from three skeleton nodes (runtime/LyingSide.h); any body, standing ones too.
        static const RE::BSFixedString com{"NPC COM [COM ]"}, neck{"NPC Neck [Neck]"}, leftArm{"NPC L UpperArm [LUar]"};
        auto* root = subject->Get3D(false);
        const auto at = [&](const RE::BSFixedString& name, LyingSide::Vec& out) {
            auto* node = root ? root->GetObjectByName(name) : nullptr;
            if (!node) return false;
            const auto& t = node->world.translate;
            out = {t.x, t.y, t.z};
            return std::isfinite(t.x) && std::isfinite(t.y) && std::isfinite(t.z);
        };
        LyingSide::Vec c, n, l;
        if (!at(com, c) || !at(neck, n) || !at(leftArm, l)) return Value::kUnavailable;
        const bool back = LyingSide::OnBack(c, n, l);
        const auto chest = LyingSide::Chest(c, n, l);
        logger::debug("[FlowTest] actor={:08X} test=lying chest=({:.2f},{:.2f},{:.2f}) back={} expected={}",
            subject->GetFormID(), chest.x, chest.y, chest.z, back, p.option);
        return boolean(back == (p.option == "back"));
    }
    case Test::kHitReceived: {
        // The last weapon hit the subject received (recorded only while such a check is loaded).
        HitHistory::Hit hit;
        float ago = 0.0f;
        if (!HitHistory::LastOf(subject->GetHandle(), hit, ago)) return Value::kFalse;
        logger::debug("[FlowTest] actor={:08X} test=hitReceived option={} ago={:.3f} power={} bash={} sneak={} "
                      "blocked={}",
            subject->GetFormID(), p.option, ago, hit.power, hit.bash, hit.sneak, hit.blocked);
        if (p.within >= 0.0f && ago > p.within) return Value::kFalse;
        return boolean(HitHistory::Matches(hit, p.option));
    }
    case Test::kPreviousAction: {
        // The last of our actions the subject finished (recorded only while such a check is loaded).
        ActionHistory::Last last;
        if (!ActionHistory::LastOf(subject->GetHandle(), last)) return Value::kFalse;
        if (std::find(p.actions.begin(), p.actions.end(), last.index) == p.actions.end()) return Value::kFalse;
        if (p.within >= 0.0f && last.ago > p.within) return Value::kFalse;
        if (p.ended == "completed" && !last.completed) return Value::kFalse;
        if (p.ended == "cancelled" && last.completed) return Value::kFalse;
        return Value::kTrue;
    }
    case Test::kFalling: {
        // The engine's own fall: in the air, the controller's vertical speed, and the height it left the ground
        // at (fallStartHeight); nothing is tracked here.
        auto* controller = subject->GetCharController();
        if (!controller || controller->context.currentState != RE::hkpCharacterStateType::kInAir) return Value::kFalse;
        RE::hkVector4 velocity;
        controller->GetLinearVelocityImpl(velocity);
        const bool rising = velocity.quad.m128_f32[2] > 0.0f;
        if (rising != (p.option == "rising")) return Value::kFalse;
        if (p.compare.empty()) return Value::kTrue;
        const double fallen = static_cast<double>(controller->fallStartHeight) - subject->GetPositionZ();
        logger::debug("[FlowTest] actor={:08X} test=falling start={:.1f} z={:.1f} fallen={:.1f}", subject->GetFormID(),
            controller->fallStartHeight, subject->GetPositionZ(), fallen);
        return numeric(fallen);
    }
    case Test::kSlope: {
        // The ground the controller stands on (its surface normal, no ray): the angle from level, and whether the
        // subject faces down or up it.
        auto* controller = subject->GetCharController();
        if (!controller || controller->context.currentState == RE::hkpCharacterStateType::kInAir)
            return Value::kUnavailable;
        const auto& n = controller->surfaceInfo.surfaceNormal.quad.m128_f32;
        if (!std::isfinite(n[0]) || !std::isfinite(n[1]) || !std::isfinite(n[2]) || n[2] <= 0.0f)
            return Value::kUnavailable;
        const double slope = std::acos(std::clamp(static_cast<double>(n[2]), 0.0, 1.0)) * 180.0 / std::numbers::pi;
        if (p.option != "any" && slope > 0.5) {
            // The normal leans downhill: facing along it is facing downhill.
            const float yaw = subject->GetAngleZ();
            const double along = std::sin(yaw) * n[0] + std::cos(yaw) * n[1];
            if ((p.option == "downhill") != (along > 0.0)) return Value::kFalse;
        }
        return numeric(slope);
    }
    case Test::kActorValue: {
        if (p.option == "level") return numeric(subject->GetLevel());
        const auto av = RE::ActorValueList::LookupActorValueByName(p.option.c_str());
        auto* values = subject->AsActorValueOwner();
        if (av == RE::ActorValue::kNone || !values) return Value::kUnavailable;
        return numeric(p.measure == "base" ? values->GetBaseActorValue(av) : values->GetActorValue(av));
    }
    case Test::kGameHour: {
        auto* calendar = RE::Calendar::GetSingleton();
        if (!calendar) return Value::kUnavailable;
        const double hour = calendar->GetHour();
        // A range past midnight (20 to 5) is the two ends of the day.
        if (p.compare == "range" && p.upper < p.value) return boolean(hour >= p.value || hour <= p.upper);
        return numeric(hour);
    }
    case Test::kWeather: {
        auto* sky = RE::Sky::GetSingleton();
        auto* weather = sky ? sky->currentWeather : nullptr;
        if (!weather) return Value::kUnavailable;
        using Flag = RE::TESWeather::WeatherDataFlag;
        const auto flag = p.option == "pleasant" ? Flag::kPleasant
                          : p.option == "cloudy" ? Flag::kCloudy
                          : p.option == "rainy"  ? Flag::kRainy
                                                 : Flag::kSnow;
        return boolean(weather->data.flags.any(flag));
    }
    case Test::kHeldTime: {
        // How long the key that started the player's action was held (charge, 10-30): the player only.
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || subject != player || !request.heldCaptured || request.heldTime < 0) return Value::kUnavailable;
        return numeric(request.heldTime);
    }
    case Test::kMovementDirection: {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || subject != player || !request.movementCaptured || request.movementDirection < 0)
            return Value::kUnavailable;
        const int expected = p.option == "forward" ? 0
                             : p.option == "back"  ? 1
                             : p.option == "left"  ? 2
                             : p.option == "right" ? 3
                                                   : 4;
        return boolean(request.movementDirection == expected);
    }
    case Test::kRig: {
        auto* race = subject->GetRace();
        if (!race) return Value::kUnavailable;
        auto* base = subject->GetActorBase();
        const auto sex = base ? base->GetSex() : RE::SEX::kNone;
        const char* path = race->behaviorGraphs[sex == RE::SEX::kFemale ? 1 : 0].GetModel();
        if (!path || !*path) path = race->behaviorGraphs[0].GetModel();
        const auto rig = path ? PairedRig::ProjectPath(path) : PairedRig::Kind::kUnsupported;
        if (rig == PairedRig::Kind::kUnsupported) return Value::kUnavailable;
        return boolean(p.option == "creature" ? PairedRig::IsCreature(rig) : rig == PairedRig::FromName(p.option));
    }
    case Test::kRace: {
        auto* race = form ? form->As<RE::TESRace>() : nullptr;
        return race && subject->GetRace() ? boolean(subject->GetRace() == race) : Value::kUnavailable;
    }
    case Test::kActorType: {
        // The player is one reference, not a keyword: true only for the player character (0.12.267).
        if (p.option == "player") return boolean(subject->IsPlayerRef());
        static constexpr std::pair<std::string_view, std::string_view> keywords[] = {{"npc", "ActorTypeNPC"},
            {"animal", "ActorTypeAnimal"}, {"creature", "ActorTypeCreature"}, {"undead", "ActorTypeUndead"},
            {"daedra", "ActorTypeDaedra"}, {"dwarven", "ActorTypeDwarven"}, {"dragon", "ActorTypeDragon"}};
        auto* race = subject->GetRace();
        auto* base = subject->GetActorBase();
        if (!race || !base) return Value::kUnavailable;
        for (const auto& [option, keyword] : keywords)
            if (option == p.option) return boolean(race->HasKeywordString(keyword) || base->HasKeywordString(keyword));
        return Value::kUnavailable;
    }
    case Test::kResource: {
        auto* values = subject->AsActorValueOwner();
        if (!values) return Value::kUnavailable;
        const auto av = p.option == "health"    ? RE::ActorValue::kHealth
                        : p.option == "stamina" ? RE::ActorValue::kStamina
                                                : RE::ActorValue::kMagicka;
        const double current = values->GetActorValue(av);
        if (!std::isfinite(current)) return Value::kUnavailable;
        if (p.measure == "current") return numeric(current);
        const double maximum = values->GetPermanentActorValue(av) +
                               subject->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, av);
        if (!std::isfinite(maximum) || maximum <= 0) return Value::kUnavailable;
        return numeric(std::clamp(current / maximum * 100.0, 0.0, 100.0));
    }
    case Test::kEssential: return boolean(subject->IsEssential());
    case Test::kProtected: return boolean(subject->IsProtected());
    case Test::kActorStatus: {
        auto* state = subject->AsActorState();
        if (!state) return Value::kUnavailable;
        if (p.option == "ragdoll") return boolean(subject->IsInRagdollState());
        if (p.option == "recovering") return boolean(state->GetKnockState() == RE::KNOCK_STATE_ENUM::kGetUp);
        if (p.option == "bleedingOut") return boolean(state->IsBleedingOut());
        if (p.option == "killmove") return boolean(subject->IsInKillMove());
        if (p.option == "staggered") return boolean(state->IsStaggered());
        if (p.option == "child") return boolean(subject->IsChild());
        if (p.option == "paired") {
            PairedSession::Ticket ticket;
            if (PairedSession::TryGetTicket(subject->GetHandle(), ticket)) return Value::kTrue;
            static const RE::BSFixedString synced("bIsSynced");
            bool value = false;
            return subject->GetGraphVariableBool(synced, value) ? boolean(value) : Value::kUnavailable;
        }
        return Value::kUnavailable;
    }
    case Test::kRelationship:
        if (p.option == "teammate") return boolean(subject->IsPlayerTeammate());
        return pairReady() ? boolean(subject->IsHostileToActor(counterpart)) : Value::kUnavailable;
    case Test::kFaction: {
        auto* faction = form ? form->As<RE::TESFaction>() : nullptr;
        return faction ? boolean(subject->IsInFaction(faction)) : Value::kUnavailable;
    }
    case Test::kDistance:
    case Test::kRelativeAngle:
    case Test::kHeightDifference: {
        if (!pairReady()) return Value::kUnavailable;
        const auto d = counterpart->GetPosition() - subject->GetPosition();
        if (!std::isfinite(d.x) || !std::isfinite(d.y) || !std::isfinite(d.z)) return Value::kUnavailable;
        if (p.test == Test::kDistance)
            return numeric(std::sqrt(double(d.x) * d.x + double(d.y) * d.y + double(d.z) * d.z));
        if (p.test == Test::kHeightDifference) return numeric(std::abs(d.z));
        if (double(d.x) * d.x + double(d.y) * d.y < 0.000001) return Value::kUnavailable;
        const double angle =
            std::remainder(std::atan2(double(d.x), double(d.y)) - subject->GetAngleZ(), 2 * std::numbers::pi);
        return numeric(std::abs(angle) * 180 / std::numbers::pi);
    }
    case Test::kBlocking: return boolean(subject->IsBlocking());
    case Test::kSprinting:
        if (p.subject == ConditionFlowXml::Subject::kSelf && request.sprintingCaptured)
            return boolean(request.sprinting);
        return boolean(subject->AsActorState()->IsSprinting());
    case Test::kSwimming: return boolean(subject->AsActorState()->IsSwimming());
    case Test::kMounted: return boolean(subject->IsOnMount());
    case Test::kLineOfSight: {
        if (!pairReady()) return Value::kUnavailable;
        bool detail = false;
        return boolean(subject->HasLineOfSight(counterpart, detail));
    }
    case Test::kScale: return numeric(subject->GetScale());
    case Test::kHeight: return numeric(subject->GetHeight());
    case Test::kScaleRatio: {
        if (!pairReady()) return Value::kUnavailable;
        const double base = counterpart->GetHeight();
        return std::isfinite(base) && base > 0 ? numeric(subject->GetHeight() / base) : Value::kUnavailable;
    }
    case Test::kEquippedKeyword:
    case Test::kWornKeyword:
    case Test::kKeyword: {
        auto* keyword = form ? form->As<RE::BGSKeyword>() : nullptr;
        if (!keyword) return Value::kUnavailable;
        if (p.test == Test::kKeyword) return boolean(subject->HasKeyword(keyword));
        if (!subject->GetActorRuntimeData().currentProcess) return Value::kUnavailable;
        const auto has = [&](RE::TESForm* item) {
            auto* kw = item ? item->As<RE::BGSKeywordForm>() : nullptr;
            return kw && kw->HasKeyword(keyword);
        };
        if (p.test == Test::kEquippedKeyword)
            return boolean((p.option != "right" && has(subject->GetEquippedObject(true))) ||
                           (p.option != "left" && has(subject->GetEquippedObject(false))));
        for (const auto& [item, entry] : subject->GetInventory())
            if (item && entry.second && entry.second->IsWorn() && has(item)) return Value::kTrue;
        return Value::kFalse;
    }
    case Test::kPerk: {
        auto* perk = form ? form->As<RE::BGSPerk>() : nullptr;
        return perk ? boolean(subject->HasPerk(perk)) : Value::kUnavailable;
    }
    case Test::kSpell: {
        auto* spell = form ? form->As<RE::SpellItem>() : nullptr;
        return spell ? boolean(subject->HasSpell(spell)) : Value::kUnavailable;
    }
    case Test::kChance: return boolean(request.Chance(flow, index, p.value));
    case Test::kInterior: {
        auto* cell = subject->GetParentCell();
        return cell ? boolean(cell->IsInteriorCell()) : Value::kUnavailable;
    }
    case Test::kLocation: {
        auto* location = form ? form->As<RE::BGSLocation>() : nullptr;
        auto* current = subject->GetCurrentLocation();
        return location && current ? boolean(current == location) : Value::kUnavailable;
    }
    default: return Value::kUnavailable;
    }
}
}
