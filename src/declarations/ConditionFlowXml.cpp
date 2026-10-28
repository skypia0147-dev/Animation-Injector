#include "ConditionFlowXml.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <string_view>
#include "runtime/PairedRig.h"

namespace AnimationInjector::ConditionFlowXml {
namespace {
using XML = pugi::xml_node;
[[noreturn]] void Fail(XML n, const std::string& message) { throw std::runtime_error(n.path() + ": " + message); }
// One of a multi-value check's values (0.12.301): it stands in for its attribute while that predicate is read.
thread_local const char* overrideKey = nullptr;
thread_local std::string overrideValue;
std::string Required(XML n, const char* name) {
    if (overrideKey && std::string_view(name) == overrideKey) return overrideValue;
    std::string v = n.attribute(name).value();
    if (v.empty()) Fail(n, std::string("missing ") + name);
    return v;
}
void Shape(XML n, std::initializer_list<std::string_view> names, bool children = false) {
    for (auto a : n.attributes())
        if (std::find(names.begin(), names.end(), a.name()) == names.end()) Fail(n, "unknown attribute");
    for (auto c : n.children())
        if ((c.type() == pugi::node_element && !children) || c.type() == pugi::node_pcdata ||
            c.type() == pugi::node_cdata)
            Fail(n, "unexpected content");
}
// One <Check>, with its not (read by the card).
Predicate ReadPredicate(XML n) {
    Shape(n, {"kind", "not", "subject", "minimum", "distance", "direction", "maxRise", "maxDrop", "maxSlope", "radius",
                 "plugin", "form", "equals", "hand", "type", "state", "option", "compare", "measure", "value", "upper",
                 "actions", "within", "ended"});
    Predicate predicate;
    const auto test = Required(n, "kind");
    if (test == "air")
        predicate.test = Test::kAir;
    else if (test == "weaponDrawn")
        predicate.test = Test::kWeaponDrawn;
    else if (test == "combat")
        predicate.test = Test::kCombat;
    else if (test == "alive")
        predicate.test = Test::kAlive;
    else if (test == "stamina")
        predicate.test = Test::kStamina;
    else if (test == "magicka")
        predicate.test = Test::kMagicka;
    else if (test == "space")
        predicate.test = Test::kSpace;
    else if (test == "ground")
        predicate.test = Test::kGround;
    else if (test == "magicEffect")
        predicate.test = Test::kMagicEffect;
    else if (test == "global")
        predicate.test = Test::kGlobal;
    else if (test == "equippedType")
        predicate.test = Test::kEquippedType;
    else if (test == "attackState")
        predicate.test = Test::kAttackState;
    else if (test == "sneaking")
        predicate.test = Test::kSneaking;
    else if (test == "female")
        predicate.test = Test::kFemale;
    else if (test == "rig")
        predicate.test = Test::kRig;
    else if (test == "race")
        predicate.test = Test::kRace;
    else if (test == "actorType")
        predicate.test = Test::kActorType;
    else if (test == "resource")
        predicate.test = Test::kResource;
    else if (test == "essential")
        predicate.test = Test::kEssential;
    else if (test == "protected")
        predicate.test = Test::kProtected;
    else if (test == "actorStatus")
        predicate.test = Test::kActorStatus;
    else if (test == "relationship")
        predicate.test = Test::kRelationship;
    else if (test == "faction")
        predicate.test = Test::kFaction;
    else if (test == "distance")
        predicate.test = Test::kDistance;
    else if (test == "relativeAngle")
        predicate.test = Test::kRelativeAngle;
    else if (test == "heightDifference")
        predicate.test = Test::kHeightDifference;
    else if (test == "blocking")
        predicate.test = Test::kBlocking;
    else if (test == "sprinting")
        predicate.test = Test::kSprinting;
    else if (test == "swimming")
        predicate.test = Test::kSwimming;
    else if (test == "mounted")
        predicate.test = Test::kMounted;
    else if (test == "lineOfSight")
        predicate.test = Test::kLineOfSight;
    else if (test == "scale")
        predicate.test = Test::kScale;
    else if (test == "scaleRatio")
        predicate.test = Test::kScaleRatio;
    else if (test == "height")
        predicate.test = Test::kHeight;
    else if (test == "equippedKeyword")
        predicate.test = Test::kEquippedKeyword;
    else if (test == "wornKeyword")
        predicate.test = Test::kWornKeyword;
    else if (test == "keyword")
        predicate.test = Test::kKeyword;
    else if (test == "perk")
        predicate.test = Test::kPerk;
    else if (test == "spell")
        predicate.test = Test::kSpell;
    else if (test == "chance")
        predicate.test = Test::kChance;
    else if (test == "interior")
        predicate.test = Test::kInterior;
    else if (test == "location")
        predicate.test = Test::kLocation;
    else if (test == "movementDirection")
        predicate.test = Test::kMovementDirection;
    else if (test == "lying")
        predicate.test = Test::kLying;
    else if (test == "previousAction")
        predicate.test = Test::kPreviousAction;
    else if (test == "falling")
        predicate.test = Test::kFalling;
    else if (test == "slope")
        predicate.test = Test::kSlope;
    else if (test == "actorValue")
        predicate.test = Test::kActorValue;
    else if (test == "gameHour")
        predicate.test = Test::kGameHour;
    else if (test == "weather")
        predicate.test = Test::kWeather;
    else
        Fail(n, "unsupported condition " + test);
    const auto oneOf = [&](const std::string& s, std::initializer_list<std::string_view> choices) {
        return std::find(choices.begin(), choices.end(), s) != choices.end();
    };
    const bool option = oneOf(test, {"rig", "actorType", "resource", "actorStatus", "relationship", "equippedKeyword",
                                        "movementDirection", "lying", "falling", "slope", "actorValue", "weather"});
    if (option) {
        predicate.option = Required(n, "option");
        bool valid = false;
        if (test == "rig")
            valid = predicate.option == "creature" ||
                    PairedRig::FromName(predicate.option) != PairedRig::Kind::kUnsupported;
        if (test == "actorType")
            valid = oneOf(
                predicate.option, {"player", "npc", "animal", "creature", "undead", "daedra", "dwarven", "dragon"});
        if (test == "resource") valid = oneOf(predicate.option, {"health", "stamina", "magicka"});
        if (test == "actorStatus")
            valid = oneOf(
                predicate.option, {"ragdoll", "recovering", "bleedingOut", "killmove", "paired", "staggered", "child"});
        if (test == "relationship") valid = oneOf(predicate.option, {"hostile", "teammate"});
        if (test == "equippedKeyword") valid = oneOf(predicate.option, {"left", "right", "either"});
        if (test == "movementDirection") valid = oneOf(predicate.option, {"forward", "back", "left", "right", "none"});
        if (test == "lying") valid = oneOf(predicate.option, {"back", "front"});
        if (test == "falling") valid = oneOf(predicate.option, {"rising", "falling"});
        if (test == "slope") valid = oneOf(predicate.option, {"downhill", "uphill", "any"});
        if (test == "actorValue")
            valid = oneOf(predicate.option,
                {"level", "Health", "Magicka", "Stamina", "OneHanded", "TwoHanded", "Marksman", "Block", "Smithing",
                    "HeavyArmor", "LightArmor", "Pickpocket", "Lockpicking", "Sneak", "Alchemy", "Speechcraft",
                    "Alteration", "Conjuration", "Destruction", "Illusion", "Restoration", "Enchanting"});
        if (test == "weather") valid = oneOf(predicate.option, {"pleasant", "cloudy", "rainy", "snow"});
        if (!valid) Fail(n, "invalid option");
    } else if (n.attribute("option"))
        Fail(n, "option is not supported by this condition");
    // A fall's distance is optional: a falling check without a value only asks rising or falling.
    const bool numeric = oneOf(test, {"resource", "distance", "relativeAngle", "heightDifference", "scale",
                                         "scaleRatio", "height", "slope", "actorValue", "gameHour"}) ||
                         (test == "falling" && n.attribute("value"));
    const auto number = [&](const char* key) {
        const auto s = Required(n, key);
        float value = 0;
        const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
        if (ec != std::errc{} || end != s.data() + s.size() || !std::isfinite(value)) Fail(n, "invalid numeric value");
        return value;
    };
    if (numeric || test == "chance") {
        predicate.value = number("value");
        float maximum = test == "relativeAngle" ? 180.0f
                        : test == "chance"      ? 100.0f
                        : test == "slope"       ? 90.0f
                        : test == "gameHour"    ? 24.0f
                                                : 1000000.0f;
        if (test == "actorValue") {
            predicate.measure = n.attribute("measure").as_string("current");
            if (!oneOf(predicate.measure, {"current", "base"})) Fail(n, "invalid actor value measure");
        } else if (test == "resource") {
            predicate.measure = Required(n, "measure");
            if (!oneOf(predicate.measure, {"current", "percent"})) Fail(n, "invalid resource measure");
            if (predicate.measure == "percent") maximum = 100;
        }
        if (predicate.value < 0 || predicate.value > maximum) Fail(n, "comparison out of range");
        if (numeric) {
            predicate.compare = Required(n, "compare");
            if (!oneOf(predicate.compare, {"lt", "le", "eq", "ge", "gt", "range"})) Fail(n, "invalid comparison");
            if (predicate.compare == "range") {
                predicate.upper = number("upper");
                // A game hour's range may wrap past midnight (20 to 5).
                if ((predicate.upper < predicate.value && test != "gameHour") || predicate.upper > maximum)
                    Fail(n, "invalid upper bound");
            } else if (n.attribute("upper"))
                Fail(n, "upper requires range");
        } else if (n.attribute("compare") || n.attribute("upper"))
            Fail(n, "chance only takes value");
    } else if (n.attribute("value") || n.attribute("compare") || n.attribute("upper"))
        Fail(n, "numeric attributes require numeric condition");
    if (test == "falling" && !numeric) predicate.compare.clear();  // rising or falling only
    if (test != "resource" && test != "actorValue" && n.attribute("measure")) Fail(n, "measure requires resource");
    // The last of our actions the subject finished (0.12.296): ids, the seconds since and how it ended.
    if (test == "previousAction") {
        const std::string list = Required(n, "actions");
        for (std::size_t at = 0; at <= list.size();) {
            const auto end = std::min(list.find(',', at), list.size());
            if (end > at) predicate.actionNames.push_back(list.substr(at, end - at));
            at = end + 1;
        }
        if (predicate.actionNames.empty()) Fail(n, "previousAction needs actions");
        if (n.attribute("within")) {
            predicate.within = n.attribute("within").as_float(-1.0f);
            if (!std::isfinite(predicate.within) || predicate.within < 0 || predicate.within > 600)
                Fail(n, "previousAction within is 0~600 seconds");
        }
        predicate.ended = n.attribute("ended").as_string("any");
        if (!oneOf(predicate.ended, {"any", "completed", "cancelled"})) Fail(n, "invalid previousAction ended");
    } else if (n.attribute("actions") || n.attribute("within") || n.attribute("ended"))
        Fail(n, "actions/within/ended require previousAction");
    if (predicate.test == Test::kEquippedType) {
        const auto hand = Required(n, "hand");
        if (hand != "left" && hand != "right") Fail(n, "invalid hand");
        predicate.leftHand = hand == "left";
        static constexpr std::string_view names[] = {"bow", "crossbow", "sword", "dagger", "axe", "mace", "greatsword",
            "battleaxe", "warhammer", "staff", "shield", "spell", "torch", "unarmed"};
        const auto type = Required(n, "type");
        auto it = std::find(std::begin(names), std::end(names), type);
        if (it == std::end(names)) Fail(n, "invalid equipped type");
        predicate.equippedType = static_cast<EquippedType>(it - std::begin(names));
    } else if (n.attribute("hand") || n.attribute("type"))
        Fail(n, "hand/type require equippedType");
    if (predicate.test == Test::kAttackState) {
        static constexpr std::string_view names[] = {"none", "draw", "swing", "hit", "nextAttack", "followThrough",
            "bash", "bowDraw", "bowAttached", "bowDrawn", "bowReleasing", "bowReleased", "bowNextAttack",
            "bowFollowThrough"};
        const auto state = Required(n, "state");
        auto it = std::find(std::begin(names), std::end(names), state);
        if (it == std::end(names)) Fail(n, "invalid attack state");
        predicate.attackState = static_cast<AttackState>(it - std::begin(names));
    } else if (n.attribute("state"))
        Fail(n, "state requires attackState");
    const std::string_view subject = n.attribute("subject").as_string("self");
    if (subject == "partner")
        predicate.subject = Subject::kPartner;
    else if (subject != "self")
        Fail(n, "invalid subject");
    // A roll, the world's hour, weather or global, the player's own movement keys, or the room where the actor
    // stands: the same whoever is asked, so a partner subject would only fail when there is no partner.
    if (predicate.subject != Subject::kSelf &&
        oneOf(test, {"chance", "gameHour", "weather", "global", "movementDirection", "space", "ground"}))
        Fail(n, test + " has no subject");
    if (oneOf(test, {"magicEffect", "global", "race", "faction", "equippedKeyword", "wornKeyword", "keyword", "perk",
                        "spell", "location"})) {
        predicate.plugin = Required(n, "plugin");
        if (predicate.plugin.find_first_of("/\\\r\n") != std::string::npos) Fail(n, "plugin must be a filename");
        const auto number = Required(n, "form");
        const auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), predicate.form, 16);
        if (ec != std::errc{} || end != number.data() + number.size() || predicate.form == 0 ||
            predicate.form > 0xFFFFFF)
            Fail(n, "invalid local hexadecimal form");
    } else if (n.attribute("plugin") || n.attribute("form"))
        Fail(n, "form requires a form condition");
    if (predicate.test == Test::kGlobal) {
        const auto number = Required(n, "equals");
        const auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), predicate.equals);
        if (ec != std::errc{} || end != number.data() + number.size() || !std::isfinite(predicate.equals))
            Fail(n, "invalid global comparison");
    } else if (n.attribute("equals"))
        Fail(n, "equals requires global condition");
    if (predicate.test == Test::kStamina || predicate.test == Test::kMagicka) {
        const auto number = Required(n, "minimum");
        const auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), predicate.minimum);
        if (ec != std::errc{} || end != number.data() + number.size() || !std::isfinite(predicate.minimum) ||
            predicate.minimum < 0 || predicate.minimum > 1000000)
            Fail(n, "invalid minimum");
    } else if (n.attribute("minimum"))
        Fail(n, "minimum requires a resource condition");
    if (predicate.test == Test::kSpace || predicate.test == Test::kGround) {
        const auto number = Required(n, "distance");
        const auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), predicate.distance);
        if (ec != std::errc{} || end != number.data() + number.size() || !std::isfinite(predicate.distance) ||
            predicate.distance < 0 || (predicate.distance == 0 && predicate.test == Test::kSpace) ||
            predicate.distance > 2000)
            Fail(n, "invalid query distance");
        // Where the actor stands, from its own heading (0.12.299).
        const std::string direction = Required(n, "direction");
        const auto side = direction.starts_with("self") ? direction.substr(4) : std::string();
        if (side == "Forward")
            predicate.direction = 0;
        else if (side == "Back")
            predicate.direction = 3.141592654f;
        else if (side == "Right")
            predicate.direction = 1.570796327f;
        else if (side == "Left")
            predicate.direction = -1.570796327f;
        else
            Fail(n, "invalid space direction");
    } else if (n.attribute("distance") || n.attribute("direction"))
        Fail(n, "space attributes require a space condition");
    if (predicate.test == Test::kGround) {
        const auto read = [&](const char* key, float& value) {
            if (!n.attribute(key)) return;
            const auto text = Required(n, key);
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (ec != std::errc{} || end != text.data() + text.size()) Fail(n, "invalid ground limit");
        };
        read("maxRise", predicate.ground.rise);
        read("maxDrop", predicate.ground.drop);
        read("maxSlope", predicate.ground.slope);
        read("radius", predicate.ground.radius);
        if (!GroundPolicy::Valid(predicate.ground)) Fail(n, "invalid ground limits");
    } else if (n.attribute("maxRise") || n.attribute("maxDrop") || n.attribute("maxSlope") || n.attribute("radius"))
        Fail(n, "ground limits require ground condition");
    return predicate;
}
}
bool ReadCheck(XML check, Predicate& output, std::string& error) {
    error.clear();
    try {
        if (std::string_view(check.name()) != "Check") Fail(check, "expected Check");
        if (const char* key = MultiValueKey(check.attribute("kind").as_string());
            key && std::string_view(check.attribute(key).value()).find(',') != std::string_view::npos)
            Fail(check, "several values: read with ReadChecks");
        output = ReadPredicate(check);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
const char* MultiValueKey(std::string_view kind) {
    if (kind == "equippedType") return "type";
    if (kind == "attackState") return "state";
    for (std::string_view k :
        {"movementDirection", "lying", "rig", "actorType", "actorStatus", "relationship", "weather"})
        if (kind == k) return "option";
    return nullptr;
}
bool ReadChecks(XML check, std::vector<Predicate>& output, std::string& error) {
    error.clear();
    std::vector<Predicate> read;
    try {
        if (std::string_view(check.name()) != "Check") Fail(check, "expected Check");
        const char* key = MultiValueKey(check.attribute("kind").as_string());
        const std::string list = key ? check.attribute(key).value() : "";
        if (!key || list.find(',') == std::string::npos) {
            read.push_back(ReadPredicate(check));
        } else {
            std::vector<std::string> values;
            for (std::size_t at = 0; at <= list.size();) {
                const auto end = std::min(list.find(',', at), list.size());
                auto value = list.substr(at, end - at);
                if (value.empty()) Fail(check, std::string("empty value in ") + key);
                if (std::find(values.begin(), values.end(), value) != values.end())
                    Fail(check, std::string("repeated value in ") + key + ": " + value);
                values.push_back(std::move(value));
                at = end + 1;
            }
            struct Reset {
                ~Reset() { overrideKey = nullptr; }
            } reset;
            overrideKey = key;
            for (const auto& v : values) {
                overrideValue = v;
                read.push_back(ReadPredicate(check));
            }
        }
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    output = std::move(read);
    return true;
}
}
