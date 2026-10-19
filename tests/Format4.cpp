// XML format 4 (docs/계획-문법개편-v4.md): keys written in place, pad buttons by name, an omitted Window,
// the unified defaults and the removed forms, each refused with what replaces it.
#include "Settings.h"
#include "declarations/XmlV2.h"
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
    std::string error;
    const ActionSpec& operator[](const char* id) const {
        for (const auto& a : actions)
            if (a.id == id) return a;
        throw std::runtime_error(std::string("no action ") + id);
    }
};
Parsed Parse(const std::string& body, const char* version = "4") {
    const std::string xml =
        std::string("<AnimationSet version='") + version + "' id='Test'>" + body + "</AnimationSet>";
    pugi::xml_document doc;
    doc.load_string(xml.c_str());
    Parsed p;
    std::vector<EventRuleSpec> events;
    std::string mod;
    p.ok = XmlV2::Compile(doc, "test.xml", p.actions, events, mod, p.error, Declarations::ResolveKeyName);
    return p;
}
// One action A (with the given children) and a plain B to connect to.
Parsed Action(const std::string& children) {
    return Parse("<Action id='A' clip='a.hkx'>" + children + "</Action><Action id='B' clip='b.hkx'/>");
}
// Refused, and the message names what to write instead.
bool Refused(const Parsed& p, const char* hint) { return !p.ok && p.error.find(hint) != std::string::npos; }
bool Near(float a, float b) { return std::fabs(a - b) < 0.0001f; }
const char* kPair = "<Pair id='P' clip='p.hkx'><Start key='G'/></Pair>";
}

int main() {
    try {
        // Keys in place: Start, Connect and Carry take key=/gamepad= without an <Inputs> entry.
        {
            const auto p = Action("<Start key='G' gamepad='A'/><Connect to='B' key='E'/>");
            Check(p.ok, "inline keys parse");
            Check(p["A"].trigger.key == 0x22 && p["A"].trigger.gamepad == 0x1000, "start key and pad button");
            Check(
                p["A"].links.size() == 1 && p["A"].links[0].key == 0x12 && p["A"].links[0].gamepad == 0, "connect key");
            const auto named = Parse("<Inputs><Input id='Use' key='G' gamepad='RB'/></Inputs>"
                                     "<Action id='A' clip='a.hkx'><Start input='Use'/></Action>");
            Check(named.ok && named["A"].trigger.key == 0x22 && named["A"].trigger.gamepad == 0x200,
                "an <Inputs> name still works, pad by name");
            Check(!Action("<Start input='Use' key='G'/>").ok, "a name and a key together are ambiguous");
            Check(Refused(Action("<Connect to='B'/>"), "key"), "a connect needs a key or an event");
            const auto carry = Parse("<Pair id='P' clip='p.hkx'><Start key='G'/>"
                                     "<Carry bone='NPC R Hand [RHnd]' targetRoot='NPC Root [Root]' putdown='D' "
                                     "key='H' gamepad='Y'/><Target rig='chicken'/></Pair>"
                                     "<Action id='D' clip='d.hkx'><CarryRelease at='0.5'/></Action>");
            Check(carry.ok && carry["P"].carry.key == 0x23 && carry["P"].carry.gamepad == 0x8000, "carry key in place");
        }
        // Pad buttons: the sixteen names, code:N for the rest; a bare number is refused with the name form.
        {
            const std::pair<const char*, std::uint32_t> names[] = {{"Up", 0x1}, {"Down", 0x2}, {"Left", 0x4},
                {"Right", 0x8}, {"Start", 0x10}, {"Back", 0x20}, {"LS", 0x40}, {"RS", 0x80}, {"LB", 0x100},
                {"RB", 0x200}, {"A", 0x1000}, {"B", 0x2000}, {"X", 0x4000}, {"Y", 0x8000}, {"LT", 9}, {"RT", 10}};
            for (const auto& [name, code] : names) {
                const auto p = Action(std::string("<Start gamepad='") + name + "'/>");
                Check(p.ok && p["A"].trigger.gamepad == code, "every pad name maps to the engine code");
            }
            Check(Action("<Start gamepad='code:300'/>")["A"].trigger.gamepad == 300, "code:N for unnamed buttons");
            Check(Refused(Action("<Start gamepad='4096'/>"), "code:"), "a bare number names the code: form");
            Check(Action("<Start gamepad='rb'/>")["A"].trigger.gamepad == 0x200, "names ignore case, like key names");
            for (const char* bad : {"Z", "LB2", "code:", "code:0", "code:65536", "code:1.5", "code:x", ""})
                Check(!Action(std::string("<Start gamepad='") + bad + "'/>").ok, "unknown pad buttons refused");
        }
        // Window: omitted = the whole clip; from omitted = 0, until omitted = the end.
        {
            const auto p = Action("<Start key='G'/><Connect to='B' key='E'/><Cancel to='vanilla:Block'/>"
                                  "<Cancel to='vanilla:Jump'><Window from='0.4'/></Cancel>"
                                  "<Cancel id='late' to='vanilla:Sprint'><Window until='end-0.2'/></Cancel>");
            Check(p.ok, "windows parse");
            const auto& a = p["A"];
            Check(
                Near(a.links[0].openAt, 0) && a.links[0].closeAt < 0, "a connect without Window takes the whole clip");
            Check(a.cancels.size() == 3 && a.cancels[0].mode == 0, "a cancel without Window is always open");
            Check(a.cancels[0].id == "cancel1" && a.cancels[1].id == "cancel2" && a.cancels[2].id == "late",
                "cancel ids default to cancelN like the editor");
            Check(a.cancels[1].mode == 2 && Near(a.cancels[1].openAt, 0.4f) && Near(a.cancels[1].closeAt, 0),
                "until defaults to the end");
            Check(Near(a.cancels[2].openAt, 0) && Near(a.cancels[2].closeAt, 0.2f), "from defaults to the start");
            const auto timed = Action("<Connect to='B' key='E'><Window from='0.45'/></Connect>");
            Check(timed.ok && Near(timed["A"].links[0].openAt, 0.45f) && Near(timed["A"].links[0].closeAt, 0) &&
                      timed["A"].links[0].closeRelativeToEnd,
                "connect from only: open to the end");
            Check(Refused(Action("<Cancel to='vanilla:Block'><Window always='true'/></Cancel>"), "Window"),
                "always is gone, the omitted Window named");
        }
        // Defaults: written-out values equal what the editor writes for a new action.
        {
            const auto p = Parse(std::string("<Action id='A' clip='a.hkx'><Start key='G'/>"
                                             "<Suppress events='Jump'/></Action>") +
                                 kPair);
            Check(p.ok, "defaults parse");
            const auto& a = p["A"];
            Check(a.condition.cooldown == 0 && a.condition.inAir == 0 && a.condition.staminaRequired == 0,
                "no cooldown, ground only, no stamina check");
            Check(Near(a.safetyDuration, 3.0f) && Near(p["P"].safetyDuration, 30.0f), "safety 3, a pair 30");
            Check(Near(p["P"].paired.range, 150) && Near(p["P"].paired.angle, 60), "pair target 150 / 60");
            Check(a.suppress.when == SuppressSpec::When::kStartable, "suppress when defaults to startable");
            const auto written = Action("<Require cooldown='0.2' stamina='30'/><Advanced safety='0.7'/>");
            Check(written.ok && Near(written["A"].condition.cooldown, 0.2f) && Near(written["A"].safetyDuration, 0.7f),
                "written values replace the defaults");
            Check(Near(written["A"].condition.staminaRequired, 30) && !written["A"].cost.enabled,
                "Require stamina only checks; spending is Cost");
            const auto spend = Action("<Cost stamina='30'/>");
            Check(
                spend.ok && spend["A"].cost.enabled && Near(spend["A"].cost.at, 0) && Near(spend["A"].cost.stamina, 30),
                "Cost without at spends at the start");
        }
        // Removed forms: each refused, naming its replacement.
        {
            Check(Refused(Parse("<Action id='A' clip='a.hkx'/>", "3"), "tools/convert_v4.py"), "format 3 refused");
            Check(Refused(Parse("<Action id='A' clip='a.hkx'/>", "2"), "tools/convert_v4.py"), "format 2 refused");
            Check(Refused(Parse("<Combo id='C' entry='S'><Step id='S' clip='a.hkx'/></Combo>"), "Connect"),
                "combo refused");
            Check(Refused(Action("<Step id='S' clip='a.hkx'/>"), "Connect"), "step refused");
            Check(Refused(Action("<Transition id='T' from='A' to='B'/>"), "Connect"), "transition refused");
            Check(Refused(Parse("<External id='C' type='action'/><Action id='A' clip='a.hkx'/>"), "0.12.238"),
                "external refused");
            Check(!Action("<QTE id='Q' at='0' key='E' step='S'/>").ok, "qte step refused");
            Check(!Action("<Advanced graph='FullBodyPlayerIdles_Behavior'/>").ok, "graph refused");
            const auto carry = [](const std::string& attrs, const std::string& hold) {
                return Parse("<Pair id='P' clip='p.hkx'><Start key='G'/>"
                             "<Carry bone='NPC R Hand [RHnd]' targetRoot='NPC Root [Root]' putdown='D' key='H' " +
                             attrs + ">" + hold + "</Carry><Target rig='chicken'/></Pair>" +
                             "<Action id='D' clip='d.hkx'><CarryRelease at='0.5'/></Action>");
            };
            Check(carry("weapon='sheathe'", "<Hold><Bone name='NPC R Clavicle [RClv]' weight='1'/></Hold>").ok,
                "carry control parses");
            Check(Refused(carry("alignStart='true'", ""), "alignStart"), "alignStart refused");
            Check(Refused(carry("", "<Hold pose='lift-end'/>"), "Hold pose"), "hold pose refused");
            Check(Refused(carry("weapon='keep'", ""), "0.12.238"), "weapon keep refused");
            Check(Refused(carry("weapon='sheathe' weaponShow='Show'", ""), "0.12.238"), "weaponShow refused");
            Check(Refused(Action("<Lock from='0' until='1'/>"), "0.12.238"), "lock window refused");
            Check(Refused(Parse("<EventRegistry><Event name='E'><Do op='lock'/></Event></EventRegistry>"), "0.12.238"),
                "lock function refused");
            Check(
                Refused(Parse("<EventRegistry><Event name='E'><Do op='unlock'/></Event></EventRegistry>"), "0.12.238"),
                "unlock function refused");
        }
        std::cout << "Format 4 checks passed: " << checks << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        return 1;
    }
}
