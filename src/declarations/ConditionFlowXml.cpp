#include "ConditionFlowXml.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <stdexcept>
#include <string_view>
#include "runtime/PairedRig.h"

namespace AnimationInjector::ConditionFlowXml {
namespace {
using XML = pugi::xml_node;
[[noreturn]] void Fail(XML n, const std::string& message) { throw std::runtime_error(n.path() + ": " + message); }
std::string Required(XML n, const char* name) {
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
void Id(XML n, const std::string& s) {
    auto letter = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
    if (s.empty() || s.size() > 64 || !letter(s[0]) ||
        !std::all_of(s.begin(), s.end(), [&](char c) { return letter(c) || (c >= '0' && c <= '9') || c == '_'; }))
        Fail(n, "invalid ID");
}
}
bool Read(XML flow, const std::vector<std::string>& actions, Program& output, std::string& error) {
    output = {};
    error.clear();
    try {
        if (std::string_view(flow.name()) != "Flow") Fail(flow, "expected Flow");
        Shape(flow, {"entry"}, true);
        Program p;
        std::map<std::string, int> ids;
        std::vector<XML> source;
        for (auto n : flow.children())
            if (n.type() == pugi::node_element) {
                if (source.size() >= ConditionFlow::limit) Fail(n, "too many flow nodes");
                const auto id = Required(n, "id");
                Id(n, id);
                if (!ids.emplace(id, static_cast<int>(source.size())).second) Fail(n, "duplicate ID");
                p.nodeIds.push_back(id);
                source.push_back(n);
                p.nodes.emplace_back();
            }
        const auto ref = [&](XML n, const std::string& id) {
            auto it = ids.find(id);
            if (it == ids.end()) Fail(n, "missing node " + id);
            return it->second;
        };
        p.entry = ref(flow, Required(flow, "entry"));
        for (std::size_t i = 0; i < source.size(); ++i) {
            const auto n = source[i];
            auto& node = p.nodes[i];
            const std::string_view kind = n.name();
            if (kind == "Test") {
                Shape(n, {"id", "kind", "subject", "minimum", "pass", "fail", "candidate", "distance", "direction",
                             "maxRise", "maxDrop", "maxSlope", "radius", "plugin", "form", "equals", "hand", "type",
                             "state", "option", "compare", "measure", "value", "upper"});
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
                else if (test == "unarmedRight")
                    predicate.test = Test::kUnarmedRight;
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
                else
                    Fail(n, "unsupported condition " + test);
                const auto oneOf = [&](const std::string& s, std::initializer_list<std::string_view> choices) {
                    return std::find(choices.begin(), choices.end(), s) != choices.end();
                };
                const bool option = oneOf(test, {"rig", "actorType", "resource", "actorStatus", "relationship",
                                                    "equippedKeyword", "movementDirection", "lying"});
                if (option) {
                    predicate.option = Required(n, "option");
                    bool valid = false;
                    if (test == "rig")
                        valid = predicate.option == "creature" ||
                                PairedRig::FromName(predicate.option) != PairedRig::Kind::kUnsupported;
                    if (test == "actorType")
                        valid = oneOf(predicate.option,
                            {"player", "npc", "animal", "creature", "undead", "daedra", "dwarven", "dragon"});
                    if (test == "resource") valid = oneOf(predicate.option, {"health", "stamina", "magicka"});
                    if (test == "actorStatus")
                        valid = oneOf(predicate.option,
                            {"ragdoll", "recovering", "bleedingOut", "killmove", "paired", "staggered", "child"});
                    if (test == "relationship") valid = oneOf(predicate.option, {"hostile", "teammate"});
                    if (test == "equippedKeyword") valid = oneOf(predicate.option, {"left", "right", "either"});
                    if (test == "movementDirection")
                        valid = oneOf(predicate.option, {"forward", "back", "left", "right", "none"});
                    if (test == "lying") valid = oneOf(predicate.option, {"back", "front"});
                    if (!valid) Fail(n, "invalid option");
                } else if (n.attribute("option"))
                    Fail(n, "option is not supported by this condition");
                const bool numeric = oneOf(test,
                    {"resource", "distance", "relativeAngle", "heightDifference", "scale", "scaleRatio", "height"});
                const auto number = [&](const char* key) {
                    const auto s = Required(n, key);
                    float value = 0;
                    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
                    if (ec != std::errc{} || end != s.data() + s.size() || !std::isfinite(value))
                        Fail(n, "invalid numeric value");
                    return value;
                };
                if (numeric || test == "chance") {
                    predicate.value = number("value");
                    float maximum = test == "relativeAngle" ? 180.0f : test == "chance" ? 100.0f : 1000000.0f;
                    if (test == "resource") {
                        predicate.measure = Required(n, "measure");
                        if (!oneOf(predicate.measure, {"current", "percent"})) Fail(n, "invalid resource measure");
                        if (predicate.measure == "percent") maximum = 100;
                    }
                    if (predicate.value < 0 || predicate.value > maximum) Fail(n, "comparison out of range");
                    if (numeric) {
                        predicate.compare = Required(n, "compare");
                        if (!oneOf(predicate.compare, {"lt", "le", "eq", "ge", "gt", "range"}))
                            Fail(n, "invalid comparison");
                        if (predicate.compare == "range") {
                            predicate.upper = number("upper");
                            if (predicate.upper < predicate.value || predicate.upper > maximum)
                                Fail(n, "invalid upper bound");
                        } else if (n.attribute("upper"))
                            Fail(n, "upper requires range");
                    } else if (n.attribute("compare") || n.attribute("upper"))
                        Fail(n, "chance only takes value");
                } else if (n.attribute("value") || n.attribute("compare") || n.attribute("upper"))
                    Fail(n, "numeric attributes require numeric condition");
                if (test != "resource" && n.attribute("measure")) Fail(n, "measure requires resource");
                if (predicate.test == Test::kEquippedType) {
                    const auto hand = Required(n, "hand");
                    if (hand != "left" && hand != "right") Fail(n, "invalid hand");
                    predicate.leftHand = hand == "left";
                    static constexpr std::string_view names[] = {"bow", "crossbow", "sword", "dagger", "axe", "mace",
                        "greatsword", "battleaxe", "warhammer", "staff", "shield", "spell", "torch", "unarmed"};
                    const auto type = Required(n, "type");
                    auto it = std::find(std::begin(names), std::end(names), type);
                    if (it == std::end(names)) Fail(n, "invalid equipped type");
                    predicate.equippedType = static_cast<EquippedType>(it - std::begin(names));
                } else if (n.attribute("hand") || n.attribute("type"))
                    Fail(n, "hand/type require equippedType");
                if (predicate.test == Test::kAttackState) {
                    static constexpr std::string_view names[] = {"none", "draw", "swing", "hit", "nextAttack",
                        "followThrough", "bash", "bowDraw", "bowAttached", "bowDrawn", "bowReleasing", "bowReleased",
                        "bowNextAttack", "bowFollowThrough"};
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
                if (oneOf(test, {"magicEffect", "global", "race", "faction", "equippedKeyword", "wornKeyword",
                                    "keyword", "perk", "spell", "location"})) {
                    predicate.plugin = Required(n, "plugin");
                    if (predicate.plugin.find_first_of("/\\\r\n") != std::string::npos)
                        Fail(n, "plugin must be a filename");
                    const auto number = Required(n, "form");
                    const auto [end, ec] =
                        std::from_chars(number.data(), number.data() + number.size(), predicate.form, 16);
                    if (ec != std::errc{} || end != number.data() + number.size() || predicate.form == 0 ||
                        predicate.form > 0xFFFFFF)
                        Fail(n, "invalid local hexadecimal form");
                } else if (n.attribute("plugin") || n.attribute("form"))
                    Fail(n, "form requires a form condition");
                if (predicate.test == Test::kGlobal) {
                    if (predicate.subject != Subject::kSelf) Fail(n, "global condition has no partner subject");
                    const auto number = Required(n, "equals");
                    const auto [end, ec] =
                        std::from_chars(number.data(), number.data() + number.size(), predicate.equals);
                    if (ec != std::errc{} || end != number.data() + number.size() || !std::isfinite(predicate.equals))
                        Fail(n, "invalid global comparison");
                } else if (n.attribute("equals"))
                    Fail(n, "equals requires global condition");
                if (predicate.test == Test::kStamina || predicate.test == Test::kMagicka) {
                    const auto number = Required(n, "minimum");
                    const auto [end, ec] =
                        std::from_chars(number.data(), number.data() + number.size(), predicate.minimum);
                    if (ec != std::errc{} || end != number.data() + number.size() ||
                        !std::isfinite(predicate.minimum) || predicate.minimum < 0 || predicate.minimum > 1000000)
                        Fail(n, "invalid minimum");
                } else if (n.attribute("minimum"))
                    Fail(n, "minimum requires a resource condition");
                if (predicate.test == Test::kSpace || predicate.test == Test::kGround) {
                    const auto candidate = Required(n, "candidate");
                    const auto it = std::find(actions.begin(), actions.end(), candidate);
                    if (it == actions.end()) Fail(n, "missing candidate action " + candidate);
                    predicate.candidate = static_cast<int>(it - actions.begin());
                    const auto number = Required(n, "distance");
                    const auto [end, ec] =
                        std::from_chars(number.data(), number.data() + number.size(), predicate.distance);
                    if (ec != std::errc{} || end != number.data() + number.size() ||
                        !std::isfinite(predicate.distance) || predicate.distance < 0 ||
                        (predicate.distance == 0 && predicate.test == Test::kSpace) || predicate.distance > 2000)
                        Fail(n, "invalid query distance");
                    const auto direction = Required(n, "direction");
                    if (direction == "partnerForward")
                        predicate.direction = 0;
                    else if (direction == "partnerBack")
                        predicate.direction = 3.141592654f;
                    else if (direction == "partnerRight")
                        predicate.direction = 1.570796327f;
                    else if (direction == "partnerLeft")
                        predicate.direction = -1.570796327f;
                    else
                        Fail(n, "invalid space direction");
                } else if (n.attribute("candidate") || n.attribute("distance") || n.attribute("direction"))
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
                } else if (n.attribute("maxRise") || n.attribute("maxDrop") || n.attribute("maxSlope") ||
                           n.attribute("radius"))
                    Fail(n, "ground limits require ground condition");
                node.kind = ConditionFlow::Kind::kTest;
                node.predicate = static_cast<int>(p.predicates.size());
                p.predicates.push_back(predicate);
            } else if (kind == "All" || kind == "Any" || kind == "Not") {
                Shape(n, {"id", "pass", "fail"}, true);
                node.kind = kind == "All"   ? ConditionFlow::Kind::kAll
                            : kind == "Any" ? ConditionFlow::Kind::kAny
                                            : ConditionFlow::Kind::kNot;
                for (auto child : n.children())
                    if (child.type() == pugi::node_element) {
                        if (std::string_view(child.name()) != "Use") Fail(child, "expected Use");
                        Shape(child, {"node"});
                        node.operands.push_back(ref(child, Required(child, "node")));
                    }
            } else if (kind == "Play") {
                Shape(n, {"id", "action"});
                const auto name = Required(n, "action");
                const auto it = std::find(actions.begin(), actions.end(), name);
                if (it == actions.end()) Fail(n, "missing action " + name);
                node.kind = ConditionFlow::Kind::kAction;
                node.action = static_cast<int>(it - actions.begin());
            } else if (kind == "Stop") {
                Shape(n, {"id"});
                node.kind = ConditionFlow::Kind::kStop;
            } else
                Fail(n, "unknown flow node");
            if (n.attribute("pass")) node.pass = ref(n, Required(n, "pass"));
            if (n.attribute("fail")) node.fail = ref(n, Required(n, "fail"));
        }
        const auto valid = ConditionFlow::Validate(p.nodes, p.entry, p.predicates.size(), actions.size());
        if (valid != ConditionFlow::Error::kNone)
            Fail(flow, "invalid graph structure (" + std::to_string(static_cast<int>(valid)) + ")");
        output = std::move(p);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
}
