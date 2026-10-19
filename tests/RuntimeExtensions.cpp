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
    return "<AnimationSet version='4' id='Test'><Action id='A' clip='a.hkx'>" + content + "</Action></AnimationSet>";
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
        for (const auto& spec : specs) Check(spec.clips.size() == 1, "one clip per action; a Flow picks the direction");
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
        for (const auto& bad : {"<Suppress events='A' when='always'/>", "<Suppress when='active'/>"})
            Check(!Valid(Wrap(bad)), "invalid extension rejected");
        Check(Valid(Wrap("<Suppress events='A'/>")), "suppress when defaults to startable");
        // Motion is the HKX's AMR annotations only (0.12.231): every former motion form names the removal.
        for (const auto& gone : {"<Motion type='keys'><Key at='0' x='0' y='0'/><Key at='1' x='0' y='-10'/></Motion>",
                 "<Motion type='none'/>", "<Motion type='annotation'/>",
                 "<Motion type='curve' distance='100' duration='0.5' direction='back'/>",
                 "<Clips><Clip file='a.hkx' direction='back'/></Clips>", "<Start event='E' direction='movement'/>",
                 "<Start event='E' defaultDirection='none'/>", "<Start event='E' strict='true'/>"})
            Check(Error(Wrap(gone)).find("0.12.231") != std::string::npos, "removed motion syntax named");
        // Combos went in format 4: each names its replacement (an Action per HKX joined by Connect).
        Check(Error("<AnimationSet version='4' id='Test'><Combo id='A' entry='First'><Step id='First' clip='a.hkx'/>"
                    "</Combo></AnimationSet>")
                      .find("Connect") != std::string::npos,
            "combo refused with its replacement");
        Check(Error(Wrap("<Step id='First' clip='a.hkx'/>")).find("Connect") != std::string::npos,
            "step inside an action refused");
        Check(Valid(Wrap("<Start event='E'/>")), "start without direction accepted");
        auto flow = [](const std::string& test) {
            return Wrap("<Flow entry='T'><Test id='T' " + test + " pass='P'/><Play id='P' action='A'/></Flow>");
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
