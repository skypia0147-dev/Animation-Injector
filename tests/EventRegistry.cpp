#include "Settings.h"
#include "declarations/XmlV2.h"
#include "runtime/EventRegistryMerge.h"
#include <iostream>
#include <stdexcept>

using namespace AnimationInjector;
namespace {
unsigned checks = 0;
void Check(bool b, const char* text) {
    ++checks;
    if (!b) throw std::runtime_error(text);
}
struct Parsed {
    bool ok = false;
    std::vector<ActionSpec> actions;
    std::vector<EventRuleSpec> events;
    std::string error;
};
Parsed Parse(const std::string& body, const char* version = "4") {
    const std::string xml =
        std::string("<AnimationSet version='") + version + "' id='Test'>" + body + "</AnimationSet>";
    pugi::xml_document doc;
    doc.load_string(xml.c_str());
    Parsed p;
    std::string mod;
    p.ok = XmlV2::Compile(doc, "test.xml", p.actions, p.events, mod, p.error, Declarations::ResolveKeyName);
    return p;
}
const char* kAction = "<Action id='A' clip='a.hkx'/>";
bool Rejected(const std::string& body, const char* version = "4") { return !Parse(body, version).ok; }
}

int main() {
    try {
        // The registry: names, functions, defaults and an event registered only for the graphs.
        {
            const auto p = Parse(std::string(kAction) + R"(<EventRegistry>
                <Event name="Grapple_Stom"><Do op="spell" plugin="FH_Grapple.esp" form="0x80B" delay="0.4"/></Event>
                <Event name="Grapple_CamShake"><Do op="shake" target="player" from="player" parameters="true"/></Event>
                <Event name="MCO_WinOpen"/>
                <Event name="Hit"><Do op="hide"/><Do op="iframeOn" target="partner" from="initiator"/></Event>
                <Event name="Fire"><Do op="fireArrow" target="player"/></Event>
            </EventRegistry>)");
            Check(p.ok, "a registry parses");
            Check(p.events.size() == 5 && p.actions.size() == 1, "every event and the action are read");
            const auto& stom = p.events[0];
            Check(stom.name == "Grapple_Stom" && stom.source == "test.xml" && stom.functions.size() == 1,
                "name, source and function");
            const auto& spell = stom.functions[0];
            Check(spell.op == EventOp::kSpell && spell.plugin == "FH_Grapple.esp" && spell.form == "80B" &&
                      std::fabs(spell.delay - 0.4f) < 1e-6f,
                "spell settings, form without 0x");
            Check(spell.target == EventTarget::kSender && spell.from == EventFrom::kAny,
                "target and from default to the sender and anyone");
            const auto& shake = p.events[1].functions[0];
            Check(shake.op == EventOp::kShake && shake.target == EventTarget::kPlayer &&
                      shake.from == EventFrom::kPlayer && shake.parameters,
                "shake with parameters for the player");
            Check(p.events[2].name == "MCO_WinOpen" && p.events[2].functions.empty(), "an event without functions");
            Check(p.events[3].functions.size() == 2 && p.events[3].functions[1].op == EventOp::kIFrameOn &&
                      p.events[3].functions[1].target == EventTarget::kPartner &&
                      p.events[3].functions[1].from == EventFrom::kInitiator,
                "several functions, pair roles");
            Check(p.events[4].functions[0].op == EventOp::kFireArrow, "fireArrow is an event function");
        }
        Check(Parse("<EventRegistry><Event name='OnlyRegistered'/></EventRegistry>").ok,
            "a file may hold only registrations");
        Check(Rejected("<EventRegistry/>") && Rejected(""), "a file needs an action or an event");
        for (const char* old : {"2", "3"}) {
            const auto p = Parse(kAction, old);
            Check(!p.ok && p.error.find("tools/convert_v4.py") != std::string::npos,
                "formats 2 and 3 are refused with the converter named");
        }
        Check(Rejected("<EventRegistry><Event name='Same'/><Event name='same'/></EventRegistry>"),
            "one name twice in a file, ignoring case");
        Check(Rejected("<EventRegistry><Event name='X'><Do op='spell'/></Event></EventRegistry>"),
            "a spell needs its plugin and form");
        Check(
            Rejected("<EventRegistry><Event name='X'><Do op='hide' plugin='a.esp' form='1'/></Event></EventRegistry>"),
            "plugin and form only where a spell is used");
        Check(Rejected("<EventRegistry><Event name='X'><Do op='fireArrow' target='partner'/></Event></EventRegistry>"),
            "fireArrow shoots the player's bow");
        Check(Rejected("<EventRegistry><Event name='X'><Do op='hide' target='lead'/></Event></EventRegistry>"),
            "lead/support are not role names (the engine's lead is the receiver)");
        Check(Rejected("<EventRegistry><Event name='X'><Do op='hide' at='1'/></Event></EventRegistry>"),
            "an event function has no clip time");
        Check(Rejected("<EventRegistry><Event name='a|b'/></EventRegistry>"), "event names are checked");

        // Removed forms: event effects, extra registrations and event moments of clip functions.
        Check(Rejected("<Pair id='P' clip='p.hkx'><Effects><Effect name='X' op='hide'/></Effects></Pair>"),
            "pair Effect elements are gone");
        Check(Rejected("<Pair id='P' clip='p.hkx'><Effects><Effect at='1' op='hide'/></Effects></Pair>"),
            "including clip-time Effect elements");
        Check(Rejected("<Action id='A' clip='a.hkx'><Events extra='X'/></Action>"), "Events extra is gone");
        Check(Rejected("<Action id='A' clip='a.hkx'><FireArrow at='event:Fire'/></Action>"),
            "an event shot is a registry function");
        Check(Rejected("<Action id='A' clip='a.hkx'><IFrame from='event:On' until='end'/></Action>"),
            "windows open at clip times");
        Check(Rejected("<Action id='A' clip='a.hkx'><HyperArmor from='0.1' until='0.5' untilFallback='end'/></Action>"),
            "windows have no event fallbacks");
        Check(Rejected("<Action id='A' clip='a.hkx'><Cost at='event:Pay' stamina='5'/></Action>"),
            "costs are paid at clip times");

        // A pair's clip-time effects.
        {
            const auto p = Parse(R"(<Pair id='P' clip='p.hkx'><Effects>
                <Camera reset='Reset' restore='event' timeout='5'/>
                <Do at='0.15' op='hide' target='both'/>
                <Do at='0.2' op='cost' target='player' stamina='10'/>
            </Effects></Pair>)");
            Check(p.ok, "timed pair effects parse");
            const auto& f = p.actions[0].pairFeatures;
            Check(f.enabled && f.cameraReset == "Reset" && !f.cameraRestoreOnEnd &&
                      std::fabs(f.cameraTimeout - 5) < 1e-6f,
                "camera settings");
            Check(f.timed.size() == 2 && std::fabs(f.timed[0].at - 0.15f) < 1e-6f &&
                      f.timed[0].function.op == EventOp::kHide && f.timed[0].function.target == EventTarget::kBoth,
                "clip-time hide for both");
            Check(f.timed[1].function.op == EventOp::kCost && std::fabs(f.timed[1].function.stamina - 10) < 1e-6f,
                "clip-time cost");
            Check(Rejected("<Pair id='P' clip='p.hkx'><Effects><Do op='hide'/></Effects></Pair>"),
                "a pair's Do needs its clip time");
            Check(Rejected("<Pair id='P' clip='p.hkx'><Effects><Do at='1' op='fireArrow'/></Effects></Pair>"),
                "a clip-time shot is the action's FireArrow");
        }
        // An ordinary action's camera restore (0.12.260): Camera alone; the other effects stay a pair's.
        {
            const auto p = Parse(
                "<Action id='A' clip='a.hkx'><Effects><Camera reset='Back' restore='event' timeout='3'/></Effects></Action>");
            Check(p.ok, "an ordinary action takes a camera restore");
            const auto& f = p.actions[0].pairFeatures;
            Check(f.cameraReset == "Back" && !f.cameraRestoreOnEnd && std::fabs(f.cameraTimeout - 3) < 1e-6f,
                "its camera settings");
            Check(Parse("<Action id='A' clip='a.hkx'><Effects><Camera/></Effects></Action>").ok,
                "restored at its end by default");
            Check(Rejected("<Action id='A' clip='a.hkx'><Effects><Do at='0.1' op='hide'/></Effects></Action>"),
                "clip-time effects stay a pair's");
            Check(Rejected("<Action id='A' clip='a.hkx'><Effects interruptOnThirdPartyHit='true'/></Action>"),
                "third-party interruption stays a pair's");
        }

        // FireArrow: a clip time and/or the button release during the action.
        {
            const auto p = Parse("<Action id='A' clip='a.hkx'><FireArrow at='0.5'/></Action>");
            Check(p.ok && p.actions[0].fireArrow.enabled && std::fabs(p.actions[0].fireArrow.at - 0.5f) < 1e-6f &&
                      p.actions[0].fireArrow.onRelease,
                "a timed shot, release on by default");
            const auto release = Parse("<Action id='A' clip='a.hkx'><FireArrow/></Action>");
            Check(release.ok && release.actions[0].fireArrow.at < 0 && release.actions[0].fireArrow.onRelease,
                "release only");
            Check(Rejected("<Action id='A' clip='a.hkx'><FireArrow onRelease='false'/></Action>"),
                "a FireArrow that never fires");
        }

        // Every file's registry merged: all functions run, equal ones once, names ignore case.
        {
            EventFunctionSpec hide;
            hide.op = EventOp::kHide;
            EventFunctionSpec spell;
            spell.op = EventOp::kSpell;
            spell.plugin = "a.esp";
            spell.form = "1";
            std::vector<EventRuleSpec> rules = {
                {"Grapple_unequip", {hide}, "Grapple.xml"},
                {"grapple_UNEQUIP", {hide}, "Combo.xml"},
                {"Grapple_unequip", {spell}, "Other.xml"},
                {"Only", {}, "Combo.xml"},
            };
            const auto merged = EventRegistryMerge::Merge(rules);
            Check(merged.entries.size() == 2, "one entry per name ignoring case");
            const auto& e = merged.entries[0];
            Check(e.name == "Grapple_unequip" && e.functions.size() == 2 && merged.merged == 1,
                "the first spelling, the equal hide once, the other file's spell kept");
            Check(e.sources == std::vector<std::string>{"Grapple.xml", "Combo.xml", "Other.xml"}, "sources in order");
            Check(merged.notes.size() == 1, "another file adding functions is noted");
            Check(merged.entries[1].name == "Only" && merged.entries[1].functions.empty(), "names without functions");
        }

        // Payload spellings: "name|value" goes into the graphs too (an annotation is sent only when its
        // whole text is a graph event name); files' values are merged once, ignoring case.
        {
            const auto p = Parse(R"(<EventRegistry>
                <Event name="Grapple_CamShake" values=" 1.5|0.5  3.0|1.0 "><Do op="shake" parameters="true"/></Event>
                <Event name="Grapple_HealthDamage"/>
            </EventRegistry>)");
            Check(p.ok && p.events[0].values == std::vector<std::string>{"1.5|0.5", "3.0|1.0"},
                "values split on spaces, in order");
            Check(GraphNames(p.events[0]) == std::vector<std::string>{"Grapple_CamShake", "Grapple_CamShake|1.5|0.5",
                                                 "Grapple_CamShake|3.0|1.0"},
                "the name, then each name|value");
            Check(GraphNames(p.events[1]) == std::vector<std::string>{"Grapple_HealthDamage"}, "no values: the name");
            Check(Rejected("<EventRegistry><Event name='X' values='1|2 1|2'/></EventRegistry>"), "a value twice");
            Check(Rejected("<EventRegistry><Event name='X' values='@SGVF'/></EventRegistry>"), "no @ in a value");
            Check(Rejected("<EventRegistry><Event name='X' values='" + std::string(256, 'v') + "'/></EventRegistry>"),
                "name|value within 256 characters");
            EventRuleSpec a{"Shake", {}, "A.xml", {"1|1", "2|2"}}, b{"shake", {}, "B.xml", {"2|2", "3|3"}};
            const auto merged = EventRegistryMerge::Merge({a, b});
            Check(
                merged.entries.size() == 1 && merged.entries[0].values == std::vector<std::string>{"1|1", "2|2", "3|3"},
                "values of every file, each once");
        }
        // An event's QTEs (0.12.248): read under <Event>, asked at once (at 0), every file's kept; no start
        // time, no recipient, ids unique within the event.
        {
            const auto p = Parse(R"(<EventRegistry>
                <Event name="Ragdoll"><QTE id="GetUp" design="converge"><Success event="Ragdoll_Recovery"/></QTE></Event>
            </EventRegistry>)");
            Check(p.ok && p.events.size() == 1 && p.events[0].qtes.size() == 1 && p.events[0].functions.empty(),
                "a QTE under an event");
            const auto& q = p.events[0].qtes[0];
            Check(q.id == "GetUp" && q.at == 0 && q.design == QteDesign::Converge &&
                      q.results[0].event == "Ragdoll_Recovery",
                "asked at once, its result event read");
            Check(Rejected("<EventRegistry><Event name='R'><QTE id='A' at='0.5'/></Event></EventRegistry>"),
                "no start time");
            Check(Rejected("<EventRegistry><Event name='R'><QTE id='A'><Success event='U' recipient='both'/></QTE>"
                           "</Event></EventRegistry>"),
                "no recipient");
            Check(Rejected("<EventRegistry><Event name='R'><QTE id='A'/><QTE id='a'/></Event></EventRegistry>"),
                "QTE ids unique within the event");
            // Asked during a pair (0.12.264): a result names the partner's ordinary action too.
            const auto withActions = [](const char* qte) {
                return std::string("<Action id='A' clip='a.hkx'/><Action id='B' clip='b.hkx'/><Pair id='P' "
                                   "clip='p.hkx'/><EventRegistry><Event name='Hit'>") +
                       qte + "</Event></EventRegistry>";
            };
            const auto both = Parse(withActions("<QTE id='Q'><Success to='A' partnerTo='B'/></QTE>"));
            Check(both.ok && both.events[0].qtes[0].results[0].to == "A" &&
                      both.events[0].qtes[0].results[0].partnerTo == "B",
                "an event QTE result names the partner's action");
            Check(Rejected(withActions("<QTE id='Q'><Success to='A' partnerTo='P'/></QTE>")),
                "the partner's action is an ordinary one");
            Check(Rejected(withActions("<QTE id='Q'><Success partnerTo='B'/></QTE>")), "partnerTo needs to");
            EventRuleSpec a{"Ragdoll", {}, "A.xml", {}, p.events[0].qtes};
            EventRuleSpec b{"ragdoll", {}, "B.xml", {}, p.events[0].qtes};
            Check(EventRegistryMerge::Merge({a, b}).entries[0].qtes.size() == 2, "every file's QTEs");
        }
        std::cout << checks << " event registry checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
