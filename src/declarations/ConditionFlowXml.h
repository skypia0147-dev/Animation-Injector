#pragma once
#include "runtime/ConditionFlow.h"
#include "core/GroundPolicy.h"
#include <pugixml.hpp>
#include <string>
#include <vector>

namespace AnimationInjector::ConditionFlowXml {
enum class Test {
    kAir,
    kWeaponDrawn,
    kCombat,
    kAlive,
    kStamina,
    kMagicka,
    kSpace,
    kGround,
    kMagicEffect,
    kGlobal,
    kUnarmedRight,
    kEquippedType,
    kAttackState,
    kSneaking,
    kFemale,
    kRig,
    kRace,
    kActorType,
    kResource,
    kEssential,
    kProtected,
    kActorStatus,
    kRelationship,
    kFaction,
    kDistance,
    kRelativeAngle,
    kHeightDifference,
    kBlocking,
    kSprinting,
    kSwimming,
    kMounted,
    kLineOfSight,
    kScale,
    kScaleRatio,
    kHeight,
    kEquippedKeyword,
    kWornKeyword,
    kKeyword,
    kPerk,
    kSpell,
    kChance,
    kInterior,
    kLocation,
    kMovementDirection,
    // On the back or on the front (0.12.253, option back|front): runtime/LyingSide.h
    kLying
};
enum class EquippedType {
    kBow,
    kCrossbow,
    kSword,
    kDagger,
    kAxe,
    kMace,
    kGreatsword,
    kBattleaxe,
    kWarhammer,
    kStaff,
    kShield,
    kSpell,
    kTorch,
    kUnarmed
};
enum class AttackState {
    kNone,
    kDraw,
    kSwing,
    kHit,
    kNextAttack,
    kFollowThrough,
    kBash,
    kBowDraw,
    kBowAttached,
    kBowDrawn,
    kBowReleasing,
    kBowReleased,
    kBowNextAttack,
    kBowFollowThrough
};
enum class Subject { kSelf, kPartner };
// Whose heading a space or ground check's direction turns from (0.12.280). kPartner is the old
// partnerForward/Back/Left/Right; kSelf reads selfForward/Back/Left/Right.
enum class DirectionBasis { kPartner, kSelf };
struct Predicate {
    Test test = Test::kAlive;
    bool leftHand = false;
    EquippedType equippedType = EquippedType::kUnarmed;
    AttackState attackState = AttackState::kNone;
    Subject subject = Subject::kSelf;
    float minimum = 0;
    int candidate = ConditionFlow::absent;
    float distance = 0;
    // Relative to the basis actor's heading on its planned start mark, before native placement.
    float direction = 0;
    DirectionBasis basis = DirectionBasis::kPartner;
    GroundPolicy::Limits ground;
    std::string plugin;
    std::uint32_t form = 0;
    float equals = 1;
    std::string option;
    std::string compare = "le";
    std::string measure = "current";
    float value = 0, upper = 0;
};
struct Program {
    std::vector<ConditionFlow::Node> nodes;
    std::vector<Predicate> predicates;
    std::vector<std::string> nodeIds;
    int entry = ConditionFlow::absent;
};
// The supplied action list is the declaration scope, not the global mod registry.
// Parsing is atomic. No partial program is returned after an error.
bool Read(pugi::xml_node flow, const std::vector<std::string>& actions, Program& output, std::string& error);
}
