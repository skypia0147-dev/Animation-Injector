#include "Settings.h"
#include "declarations/XmlV2.h"
#include "runtime/DeclarationCatalog.h"
#include "runtime/EquipmentCondition.h"
#include "runtime/SuppressionPolicy.h"
#include <iostream>
#include <stdexcept>

using namespace AnimationInjector;
namespace {
unsigned checks = 0;
void Check(bool value, const char* name) {
    ++checks;
    if (!value) throw std::runtime_error(name);
}
bool Near(float a, float b) { return std::fabs(a - b) < 0.002f; }
std::string Wrap(const std::string& content) {
    return "<AnimationSet version='6' id='Test'><Action id='A' clip='a.hkx'>" + content + "</Action></AnimationSet>";
}
// A start of A with these attributes and children (at the root).
std::string Start(const std::string& attrs, const std::string& children = "") {
    return "<AnimationSet version='6' id='Test'><Start " + attrs + " to='A'>" + children +
           "</Start><Action id='A' clip='a.hkx'/></AnimationSet>";
}
std::string Error(const std::string& xml) {
    pugi::xml_document in;
    std::vector<ActionSpec> out;
    std::string mod, error;
    Check(in.load_string(xml.c_str()), "fixture XML");
    return XmlV2::Compile(in, "test", out, mod, error, [](const char*) -> std::uint32_t { return 34; }) ? std::string()
           : error.empty() ? std::string("?")
                           : error;
}
bool Valid(const std::string& xml) { return Error(xml).empty(); }
}
int wmain(int argc, wchar_t** argv) {
    try {
        spdlog::set_level(spdlog::level::warn);
        Check(argc == 2, "port path required");
        std::vector<ActionSpec> specs;
        std::vector<EventRuleSpec> events;
        Check(Declarations::LoadXmlFile(argv[1], specs, events), "actual port parses");
        Check(specs.size() == 7 && events.size() == 4, "dispatcher, six direction actions, four registered events");
        const auto& normal = specs[0];
        Check(normal.suppress.events.size() == 2 && normal.suppress.when == SuppressSpec::When::kStartable,
            "suppress contract");
        for (const auto& spec : specs)
            Check(spec.SelectsOnly() || spec.clips.size() == 1, "one clip per action; a Flow picks the direction");
        DeclarationCatalog catalog;
        std::vector<std::string> registered;
        for (const auto& rule : events) registered.push_back(rule.name);
        catalog.Build(specs, registered);
        for (const auto& name : registered)
            Check(std::count(catalog.uniqueEvents.begin(), catalog.uniqueEvents.end(), name) == 1,
                "registered names reach the graphs once");
        Check(NativeAttackState(ConditionFlowXml::AttackState::kBowDraw) == RE::ATTACK_STATE_ENUM::kBowDraw,
            "engine enum gap respected");
        Check(NativeAttackState(ConditionFlowXml::AttackState::kBowFollowThrough) ==
                  RE::ATTACK_STATE_ENUM::kBowFollowThrough,
            "last attack state");
        for (const auto& bad : {"event='E' block='A' blockWhen='always'", "event='E' blockWhen='active'"})
            Check(!Valid(Start(bad)), "invalid extension rejected");
        Check(Valid(Start("event='E' block='A'")), "a start's block defaults to while it can start");
        // Press kinds (10-30, runtime/PressKinds.h): keys and game controls; a menu control only down.
        for (const auto& ok : {"key='G' press='tap'", "key='G' press='double'", "control='jump' press='hold'",
                 "control='wait' press='down'", "key='G' press='down'"})
            Check(Valid(Start(ok)), "press kind accepted");
        for (const auto& bad : {"key='G' press='long'", "event='E' press='tap'", "control='wait' press='tap'",
                 "control='favorites' press='hold'", "control='tweenMenu' press='double'", "key='G' onKeyUp='true'"})
            Check(!Valid(Start(bad)), "press kind rejected");
        Check(!Valid(Wrap("<Suppress events='A'/>")), "no Suppress element (a block is an attribute)");
        // Charge (10-30): a loop, on="end" and on="release" transitions, the held time condition.
        const auto two = [](const std::string& first, const std::string& content, const std::string& attrs = "") {
            return "<AnimationSet version='6' id='Test'><Action id='A' clip='a.hkx' " + attrs + ">" + content +
                   "</Action><Action id='B' clip='b.hkx' " + first + "/></AnimationSet>";
        };
        for (const auto& ok : {two("loop='true'", "<Transition to='B' on='end' blend='0.1'/>"),
                 two("loop='true' maxHold='5'", "<Transition to='B' on='release' from='0.2'/>"),
                 two("", "<Transition to='B' on='release' key='G'/><Transition to='B' key='G'/>"),
                 two("", "<Transition to='B' on='release' until='1'/><Transition to='B' on='release' from='1'/>"),
                 two("", "<Exit at='end-0.2'/><Transition to='B' on='end'/>"),
                 two("", "<Transition to='B' on='release'/><HyperArmor/>", "loop='true'"),
                 two("loop='true'", "<Transition to='B' on='move'/><Transition to='B' on='release'/>", "loop='true'"),
                 two("", "<Transition to='B' on='move' from='0.3' until='1'/>"),
                 two("", "<Transition to='B' on='move' from='0.3' until='end-0.1'/>", "loop='true'"),
                 two("", "<Transition to='B' on='move'/>", "loop='true'")})
            Check(Valid(ok), "charge accepted");
        for (const auto& bad :
            {two("", "<Transition to='B' on='end' key='G'/>"), two("", "<Transition to='B' on='end' from='0.2'/>"),
                two("", "<Transition to='B' on='end'/><Transition to='B' on='end'/>"),
                two("", "<Transition to='B' on='end'/>", "loop='true'"),
                two("", "<Exit at='event:Done'/><Transition to='B' on='end'/>"),
                two("", "<Transition to='B' on='release' event='E'/>"),
                two("", "<Transition to='B' on='release' buffer='false'/>"), two("", "<Transition to='B' on='press'/>"),
                two("", "", "maxHold='3'"), two("", "<Exit at='end-0.1'/>", "loop='true'"),
                two("", "<Transition to='B' on='move' key='G'/>"), two("", "<Transition to='B' on='move' event='E'/>"),
                two("", "<HyperArmor until='end-0.2'/>", "loop='true'"), two("", "", "align='camera'")})
            Check(!Valid(bad), "charge rejected");
        Check(Valid(Start("key='G'", "<Check kind='heldTime' compare='ge' value='1.5'/>")), "held time accepted");
        Check(!Valid(Start("key='G'", "<Check kind='heldTime' subject='partner' compare='ge' value='1.5'/>")),
            "held time has no subject");
        Check(Valid("<AnimationSet version='6' id='Test'><Action id='A' clip='a.hkx' block='A'/></AnimationSet>"),
            "an action keeps its while-playing block");
        // Motion is the HKX's AMR annotations only (0.12.231): there is no motion syntax.
        for (const auto& gone : {"<Motion type='keys'><Key at='0' x='0' y='0'/><Key at='1' x='0' y='-10'/></Motion>",
                 "<Motion type='none'/>", "<Motion type='annotation'/>",
                 "<Motion type='curve' distance='100' duration='0.5' direction='back'/>",
                 "<Clips><Clip file='a.hkx' direction='back'/></Clips>"})
            Check(!Valid(Wrap(gone)), "no motion syntax");
        for (const auto& gone :
            {"event='E' direction='movement'", "event='E' defaultDirection='none'", "event='E' strict='true'"})
            Check(!Valid(Start(gone)), "no start direction");
        // Combos went in format 4 (an Action per HKX joined by a Transition).
        Check(!Valid("<AnimationSet version='6' id='Test'><Combo id='A' entry='First'><Step id='First' clip='a.hkx'/>"
                     "</Combo></AnimationSet>"),
            "combo refused");
        Check(!Valid(Wrap("<Step id='First' clip='a.hkx'/>")), "step inside an action refused");
        Check(Valid(Start("event='E'")), "start without direction accepted");
        auto flow = [](const std::string& test) {
            return "<AnimationSet version='6' id='Test'><Start id='S' event='E' to='A'><Check " + test +
                   "/></Start><Action id='A' clip='a.hkx'/></AnimationSet>";
        };
        for (auto type : {"bow", "crossbow", "sword", "dagger", "axe", "mace", "greatsword", "battleaxe", "warhammer",
                 "staff", "shield", "spell", "torch", "unarmed"})
            for (auto hand : {"left", "right"})
                Check(Valid(flow("kind='equippedType' hand='" + std::string(hand) + "' type='" + type + "'")),
                    "all equipped names");
        for (auto state : {"none", "draw", "swing", "hit", "nextAttack", "followThrough", "bash", "bowDraw",
                 "bowAttached", "bowDrawn", "bowReleasing", "bowReleased", "bowNextAttack", "bowFollowThrough"})
            Check(Valid(flow("kind='attackState' state='" + std::string(state) + "'")), "all attack states");
        Check(Valid(flow("kind='sneaking' subject='partner'")), "sneaking accepts common attributes");
        for (auto test :
            {"kind='equippedType' type='bow'", "kind='equippedType' hand='left' type='7'",
                "kind='attackState' state='7'", "kind='attackState'", "kind='sneaking' state='none'",
                "kind='sneaking' minimum='1'", "kind='sneaking' hand='left'", "kind='sneaking' plugin='x.esp' form='1'",
                "kind='attackState' state='none' equals='1'", "kind='alive' hand='left' type='bow'"})
            Check(!Valid(flow(test)), "invalid condition attributes rejected");
        InputAcceptance input;
        input.Begin(2);
        Check(input.Result(0) == InputAcceptance::unseen, "native before request has no stale acceptance");
        input.Record(0, 1, true);
        Check(input.Result(0) == 1, "source retains selected flow destination");
        input.Begin(2);
        input.End();
        Check(input.Result(0) == 1, "nested scope retains outer acceptance");
        input.End();
        Check(input.Result(0) == InputAcceptance::unseen, "no timed suppression after dispatch");
        input.Begin(2);
        input.Record(0, 1, false);
        Check(input.Result(0) == InputAcceptance::failed, "request failure explicit");
        input.Reset(2);
        Check(input.Result(0) == InputAcceptance::unseen, "load clears acceptance");
        std::cout << "Runtime extension checks passed: " << checks << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        return 1;
    }
}
