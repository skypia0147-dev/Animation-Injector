#pragma once
#include "runtime/ConditionFlow.h"
#include "core/GroundPolicy.h"
#include <pugixml.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace AnimationInjector::ConditionFlowXml {
enum class Test {
    kAir,
    kWeaponDrawn,
    kCombat,
    kAlive,
    kSpace,
    kGround,
    kMagicEffect,
    kGlobal,
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
    kLying,
    // 0.12.296. The last of our actions the subject finished (runtime/ActionHistory.h): actions, within, ended.
    kPreviousAction,
    // In the air, rising or falling (option), and optionally how far it has fallen (compare, value).
    kFalling,
    // The ground's slope under the subject in degrees (compare, value), downhill, uphill or either way it faces.
    kSlope,
    // An actor value by name, or the level (option), current or base (measure), compared (compare, value).
    kActorValue,
    // The game's hour, 0~24 (compare, value; a range may wrap past midnight).
    kGameHour,
    // The current weather's class (option pleasant|cloudy|rainy|snow).
    kWeather,
    // 10-29. The last weapon hit the subject received (runtime/HitHistory.h): option normal|power|bash|sneak|blocked,
    // within (seconds since, absent = any time).
    kHitReceived,
    // 10-30, charge. How long the key that started the player's action was held, in seconds (compare, value):
    // ConditionValues::Request::heldTime. The player only, like movementDirection.
    kHeldTime
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
struct Predicate {
    Test test = Test::kAlive;
    bool leftHand = false;
    EquippedType equippedType = EquippedType::kUnarmed;
    AttackState attackState = AttackState::kNone;
    Subject subject = Subject::kSelf;
    float distance = 0;
    // A room or ground check's direction from the actor's current heading (0.12.299: measured where it stands, for
    // no particular action).
    float direction = 0;
    GroundPolicy::Limits ground;
    std::string plugin;
    std::uint32_t form = 0;
    // A keyword check named by its editor ID (keyword=) instead of plugin and form: keywords keep their editor ID in
    // game, unlike most forms. `keywordForm` is the keyword found once the game data is loaded (ActionRegistry), 0
    // until then or when no loaded plugin has it.
    std::string keyword;
    std::uint32_t keywordForm = 0;
    float equals = 1;
    std::string option;
    std::string compare = "le";
    std::string measure = "current";
    float value = 0, upper = 0;
    // kPreviousAction: the action ids as written, their indices once every declaration is loaded
    // (runtime/ActionRegistry.cpp), the seconds since it ended (< 0 = any) and how it ended. kHitReceived: within.
    std::vector<std::string> actionNames;
    std::vector<int> actions;
    float within = -1.0f;
    std::string ended = "any";
};
struct Program {
    std::vector<ConditionFlow::Node> nodes;
    std::vector<Predicate> predicates;
    std::vector<std::string> nodeIds;
    int entry = ConditionFlow::absent;
};
// One <Check> (subject defaults to self). Atomic: on failure `output` is left as it was and `error` holds the path and
// the reason.
bool ReadCheck(pugi::xml_node check, Predicate& output, std::string& error);
// A check that takes several values (0.12.301): `type`, `state` or `option` as a comma list, true when any value
// holds. The attribute that may list values for a condition kind, or null.
const char* MultiValueKey(std::string_view kind);
// One <Check> as one predicate per value (one for a single value). Atomic like ReadCheck.
bool ReadChecks(pugi::xml_node check, std::vector<Predicate>& output, std::string& error);
}
