#pragma once
#include <cstdint>

// A strike (EventOp::kStrike, docs/mockup-strike): damage worked out the engine's way and applied to chosen actors,
// without the engine's HitFrame cone. No engine headers (parser, checker and tests).
//
//   <Do at="0.42" op="strike" pick="area" radius="250" angle="360" up="150" down="60" ratio="1.2"/>
//   <Do at="0.42" op="strike" pick="partner" ratio="1.5" block="ignore"/>
//
// The actor the function changes (EventFunctionSpec::target: the sender, or a pair's role) is the attacker; `pick`
// chooses whom it hits. Ragdolled actors are never hit (as the engine's own melee hit), nor the attacker.
namespace AnimationInjector {
struct StrikeSpec {
    // Whom it hits. kPartner: the attacker's pair partner. kCombatTarget: its combat target. kCrosshair: the
    // player's crosshair actor (the player only). kArea: every actor in the area below.
    enum class Pick : std::uint8_t { kPartner, kCombatTarget, kCrosshair, kArea };
    Pick pick = Pick::kCombatTarget;

    // kArea. A wedge around the attacker's facing: `radius` (units, horizontal), `angle` (degrees, the whole
    // width: 60 = 30 to each side, 360 = a circle). Height from the attacker's feet: a target's position (feet)
    // from -`down` to +`up`. A target behind a wall (no line of sight from the attacker) is never hit.
    float radius = 150.0f, angle = 60.0f, up = 150.0f, down = 60.0f;
    bool hostileOnly = true;  // kArea: only actors hostile to the attacker; false = every actor but the attacker
    // kArea: the share left at the radius, 0..1 (1 = no falloff). Damage and the reaction's strength fall
    // linearly from 100% at the attacker to `edge` at the radius.
    float edge = 1.0f;

    // How much. kWeapon: the engine's own hit for the attacker's weapon in `hand` (HitData::Populate: weapon,
    // skill, perks, the target's armour; unarmed when the hand holds no melee weapon) times `ratio`. kFixed:
    // `amount` health. Either goes in as the attacker's damage (Actor::DoDamage, with the game's difficulty): a
    // killing blow kills, an essential actor goes down as the engine decides.
    enum class Base : std::uint8_t { kWeapon, kFixed };
    enum class Hand : std::uint8_t { kRight, kLeft, kBoth };  // kBoth: the two hands' hits added
    Base base = Base::kWeapon;
    Hand hand = Hand::kRight;
    float ratio = 1.0f;    // kWeapon
    float amount = 10.0f;  // kFixed
    // A target blocking toward the attacker: kVanilla reduces it as the engine does (Populate's percentBlocked),
    // kIgnore takes the unblocked damage.
    enum class Block : std::uint8_t { kVanilla, kIgnore };
    Block block = Block::kVanilla;
    // A window: > 0 keeps the area (or the picked actor) for that many seconds and hits each actor once, the
    // moment it is in; 0 = once, at the moment the function runs.
    float duration = 0.0f;

    // After the damage (0.12.294), scaled by the falloff. Nothing for a target the hit killed.
    //   reaction="stagger" strength="0~1": the vanilla stagger. The target's graph gets staggerMagnitude = `stagger`
    //     (0 small .. 1 large, the stagger blends' axis) and staggerDirection = where the attacker stands, as a turn
    //     from the target's facing (0 in front: staggers back, 0.5 behind: staggers forward; the blend is
    //     cyclic), then staggerStart. Skipped for an actor one of our windows holds out of stagger (armorOn).
    //   reaction="push" force="0~20": the engine's ragdoll pushed away from the attacker (as EventOp::kRagdoll; a
    //     pair's actor first leaves the pair's physics hold).
    enum class Reaction : std::uint8_t { kNone, kStagger, kPush };
    Reaction reaction = Reaction::kNone;
    float stagger = 0.5f;  // kStagger
    float force = 5.0f;    // kPush
    // What a real hit brings along (0.12.294), each on unless `without` names it (without="poison,hitEvent").
    // Only with the weapon's damage (Base::kWeapon) and a melee weapon in the hand; unarmed has none but impact
    // (the race's unarmed impact set) and hitEvent.
    bool enchantment = true;  // the weapon's enchantment, cast on the target as the attacker's; the player's
                              // weapon spends its charge (Right/LeftItemCharge) and does nothing when empty
    bool poison = true;       // the weapon's poison, cast the same way; one dose spent, the last one removes it
    bool perks = true;        // the attacker's "Apply Combat Hit Spell" perk entries (weapon, target), cast the same
    bool impact = true;       // the weapon's impact data set at the target's spine: hit sound, blood, decal
    bool hitEvent = true;     // a TESHitEvent (attacker, target, weapon, blocked): other mods and Papyrus OnHit

    bool operator==(const StrikeSpec&) const = default;
};
}  // namespace AnimationInjector
