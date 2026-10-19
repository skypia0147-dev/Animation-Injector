#include "PCH.h"
#include "runtime/CarryWeaponCover.h"
#include "runtime/WeaponCull.h"
#include <array>
#include <string>

namespace AnimationInjector::CarryWeaponCover {
namespace {
struct Cover {
    RE::ActorHandle actor;
    WeaponCull::Owner owner = 0;
    bool active = false;
};
std::array<Cover, 2> covers;  // [0] carrier, [1] target
CarryWeapon mode = CarryWeapon::kSheathed;
// weapon="sheathe": sheathed = the carrier's weapon is away, so drawing one drops the target (as
// weapon="sheathed"); sheatheTried = the lift's start has put it away (or failed to).
bool sheathed = false, sheatheTried = false, defaultStateWarned = false;

// weapon="sheathe" until the weapon is away: an attack (melee, bow or bash) or a spell being cast drops
// what is carried.
bool Attacking(RE::Actor* actor) {
    auto* actorState = actor ? actor->AsActorState() : nullptr;
    return actorState && (actorState->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone || actor->WhoIsCasting() != 0);
}
// A node the lift pair also hides (Effect op="hide") stays hidden until both have let it go.
void Hide(Cover& cover) {
    if (!cover.owner) cover.owner = WeaponCull::NewOwner();
    auto actor = cover.actor.get();
    WeaponCull::Hide(cover.owner, actor.get());
}
void Show(Cover& cover, const char* reason) {
    if (!cover.active) return;
    const auto nodes = WeaponCull::Count(cover.owner);
    WeaponCull::Show(cover.owner);
    auto actor = cover.actor.get();
    logger::info("[CarryWeapon] shown actor={:08X} reason={} nodes={}", actor ? actor->GetFormID() : 0, reason, nodes);
    cover = {};
}
// The node the first weapon found hangs from: the hand ('WEAPON', 'SHIELD') or a sheath.
const char* WeaponParent(RE::Actor* actor) {
    auto biped = actor && actor->Is3DLoaded() ? actor->GetActorRuntimeData().biped : nullptr;
    if (!biped) return "";
    for (auto slot : {RE::BIPED_OBJECTS::kOneHandSword, RE::BIPED_OBJECTS::kOneHandDagger,
             RE::BIPED_OBJECTS::kOneHandAxe, RE::BIPED_OBJECTS::kOneHandMace, RE::BIPED_OBJECTS::kTwoHandMelee,
             RE::BIPED_OBJECTS::kStaff, RE::BIPED_OBJECTS::kBow, RE::BIPED_OBJECTS::kCrossbow}) {
        auto& node = biped->objects[static_cast<std::size_t>(slot)].partClone;
        if (node && node->parent) return node->parent->name.c_str() ? node->parent->name.c_str() : "";
    }
    return "";
}
// weapon="sheathe": puts the carrier's weapon away at once as the lift starts, with no animation.
// Vanilla states that sheathe without one (swimming, hand-to-hand and magic unequip, a DLC02 kill
// move) send weaponSheathe and the engine's handler moves the weapons to their sheaths. The carrier
// gets the same events here, the way FireArrow delivers arrowRelease.
void SheatheNow(RE::Actor* carrier) {
    static const RE::BSFixedString begin("BeginWeaponSheathe"), sheathe("weaponSheathe"), empty;
    sheatheTried = true;
    const auto before = carrier->AsActorState()->GetWeaponState();
    const std::string from = WeaponParent(carrier);
    int states[2]{};
    int i = 0;
    for (const auto* tag : {&begin, &sheathe}) {
        RE::BSAnimationGraphEvent event{*tag, carrier, empty};
        static_cast<RE::BSTEventSink<RE::BSAnimationGraphEvent>*>(carrier)->ProcessEvent(&event, nullptr);
        states[i++] = static_cast<int>(carrier->AsActorState()->GetWeaponState());
    }
    sheathed = !WeaponOut(carrier);
    if (sheathed) {
        Show(covers[0], "sheathed");
        logger::info(
            "[CarryWeapon] sheathed at lift start weaponState={}->{}->{} weapon '{}'->'{}'; drawing a weapon or spell now drops the target",
            static_cast<int>(before), states[0], states[1], from, WeaponParent(carrier));
    } else
        logger::warn(
            "[CarryWeapon] sheathe at lift start failed weaponState={}->{}->{} weapon '{}'->'{}'; weapons stay hidden and attacking drops the target",
            static_cast<int>(before), states[0], states[1], from, WeaponParent(carrier));
}
// Default_Behavior restarts from currentDefaultState when the lift hands back to it
// (START_STATE_MODE_SYNC). weaponSheathe_EEM would set MT_Behavior_State (0), but the handler ran
// outside the graph. Written every lifting frame: the state machine rewrites it while it still
// blends out into the lift.
void HoldSheathedDefault(RE::Actor* carrier) {
    static const RE::BSFixedString variable("currentDefaultState");
    if (!sheathed || carrier->SetGraphVariableInt(variable, 0) || defaultStateWarned) return;
    defaultStateWarned = true;
    logger::warn("[CarryWeapon] graph has no currentDefaultState; the lift may return to a drawn stance");
}
}
bool WeaponOut(RE::Actor* actor) {
    auto* actorState = actor ? actor->AsActorState() : nullptr;
    return actorState && actorState->GetWeaponState() != RE::WEAPON_STATE::kSheathed;
}
void Begin(CarryWeapon weapon, RE::ActorHandle carrier, RE::ActorHandle target) {
    const auto carrierActor = carrier.get();
    mode = weapon;
    sheatheTried = defaultStateWarned = false;
    // weapon="sheathe" started sheathed is weapon="sheathed" from the lift on.
    sheathed = weapon == CarryWeapon::kSheathe && carrierActor && !WeaponOut(carrierActor.get());
    for (auto& cover : covers) Show(cover, "new-carry");
    if (weapon == CarryWeapon::kSheathed) return;
    // The carrier's weapon is hidden only while it is out; a sheathed one stays on the belt.
    const bool carrierOut = WeaponOut(carrierActor.get());
    covers[0].actor = carrier;
    covers[1].actor = target;
    for (std::size_t i = carrierOut ? 0 : 1; i < covers.size(); ++i) {
        covers[i].active = true;
        Hide(covers[i]);
    }
    logger::info("[CarryWeapon] hidden while carried (sheathe); carrier {}",
        carrierOut ? "shows once the lift's start has sheathed it" : "already sheathed");
}
void Lifting(RE::Actor* carrier, bool liftPlaying) {
    if (mode == CarryWeapon::kSheathe && !sheathed && !sheatheTried && liftPlaying) SheatheNow(carrier);
    HoldSheathedDefault(carrier);
}
const char* Rule(RE::Actor* carrier) {
    if (mode == CarryWeapon::kSheathe && !sheathed) return Attacking(carrier) ? "carrier-attack" : nullptr;
    return WeaponOut(carrier) ? "carrier-weapon-out" : nullptr;
}
void End(const char* reason) {
    for (auto& cover : covers) Show(cover, reason);
    mode = CarryWeapon::kSheathed;
    sheathed = sheatheTried = defaultStateWarned = false;
}
void Maintain() {
    for (auto& cover : covers)
        if (cover.active) Hide(cover);
}
void Reset(const char* reason) {
    for (auto& cover : covers) Show(cover, reason);
    mode = CarryWeapon::kSheathed;
    sheathed = sheatheTried = defaultStateWarned = false;
}
}
