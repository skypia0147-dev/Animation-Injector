// XML format 6 (docs/설계-형식6-조건카드.md): starts and condition cards read straight into the runtime's
// declarations. The cards (K1-K7) pick as written, the starts land on the actions as T1-T7 say, the attributes and
// renamed elements of 1.5 read into the same fields.
#include "Settings.h"
#include "declarations/XmlV2.h"
#include "runtime/ConditionFlow.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>
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
    const ActionSpec* find(const char* id) const {
        for (const auto& a : actions)
            if (a.id == id) return &a;
        return nullptr;
    }
    const ActionSpec& operator[](const char* id) const {
        if (auto* a = find(id)) return *a;
        throw std::runtime_error(std::string("no action ") + id + (ok ? "" : ": " + error));
    }
};
Parsed Parse(const std::string& body, const char* version = "6") {
    const std::string xml =
        std::string("<AnimationSet version='") + version + "' id='Test'>" + body + "</AnimationSet>";
    pugi::xml_document doc;
    doc.load_string(xml.c_str());
    Parsed p;
    std::string mod;
    p.ok = XmlV2::Compile(doc, "test.xml", p.actions, p.events, mod, p.error, Declarations::ResolveKeyName);
    return p;
}
bool Refused(const Parsed& p, const char* hint) { return !p.ok && p.error.find(hint) != std::string::npos; }
bool Near(float a, float b) { return std::fabs(a - b) < 0.0001f; }
// A condition that is true when its number is in the set Pick is given.
std::string G(int n, const char* extra = "") {
    return "<Check kind='global' plugin='t.esp' form='" + std::to_string(n) + "' equals='1'" + extra + "/>";
}
// What a selecting action's program picks when the conditions numbered in `on` hold ("" for nothing).
std::string Pick(const ActionSpec& s, std::set<unsigned> on) {
    const auto r =
        ConditionFlow::Select(s.flow.nodes, s.flow.entry, s.flow.predicates.size(), s.flowActions.size(), [&](int i) {
            const auto& p = s.flow.predicates[i];
            return on.contains(p.form) ? ConditionFlow::Value::kTrue : ConditionFlow::Value::kFalse;
        });
    if (r.error != ConditionFlow::Error::kNone) throw std::runtime_error("flow evaluation failed");
    return r.action < 0 ? std::string() : s.flowActions[r.action];
}
const char* kABC = "<Action id='A' clip='a.hkx'/><Action id='B' clip='b.hkx'/><Action id='C' clip='c.hkx'/>";
}

int main() {
    try {
        // The card (1.3): common conditions, then the first row that holds, else False.
        {
            const auto p = Parse("<Start id='S' key='G'>" + G(1) + "<Row to='A'>" + G(2) + "</Row><Row to='B'>" + G(3) +
                                 "</Row><False to='C'/></Start>" + kABC);
            Check(p.ok, p.error.c_str());
            const auto& s = p["S"];
            Check(s.SelectsOnly() && s.trigger.key == 0x22, "a card start is a selecting action with the trigger");
            Check(Pick(s, {1, 2, 3}) == "A", "the first row that holds");
            Check(Pick(s, {1, 3}) == "B", "a failing row tries the next");
            Check(Pick(s, {1}) == "C", "no row holds: False");
            Check(Pick(s, {2, 3}) == "C", "a common condition fails: False");
        }
        {
            const auto p = Parse("<Start id='S' key='G'>" + G(1) + "<True to='A'/></Start>" + kABC);
            Check(p.ok && Pick(p["S"], {1}) == "A" && Pick(p["S"], {}).empty(), "True without False plays nothing");
            const auto n = Parse("<Start id='S' key='G'>" + G(1, " not='true'") + "<True to='A'/></Start>" + kABC);
            Check(n.ok && Pick(n["S"], {}) == "A" && Pick(n["S"], {1}).empty(), "not='true' negates");
            const auto g = Parse("<Start id='S' key='G'><Or>" + G(1) + G(2) + "</Or><And not='true'>" + G(3) + G(4) +
                                 "</And><True to='A'/></Start>" + kABC);
            Check(g.ok && Pick(g["S"], {2}) == "A" && Pick(g["S"], {1, 3}) == "A" && Pick(g["S"], {1, 3, 4}).empty() &&
                      Pick(g["S"], {3}).empty(),
                "And/Or groups, negated");
        }
        // Destinations: to with common conditions, an inner card, a shared card (copied per program, K6).
        {
            const auto p = Parse("<Condition id='Dir'><Row to='A'>" + G(5) + "</Row><False to='B'/></Condition>" +
                                 "<Start id='S1' key='G' to='C'>" + G(1) + "</Start>" + "<Start id='S2' key='H'><Row>" +
                                 G(2) + "<Condition>" + G(3) +
                                 "<True to='A'/><False condition='Dir'/></Condition></Row><False condition='Dir'/>"
                                 "</Start>" +
                                 "<Start id='S3' key='J' condition='Dir'/>" + kABC);
            Check(p.ok, p.error.c_str());
            Check(Pick(p["S1"], {1}) == "C" && Pick(p["S1"], {}).empty(), "to with common conditions");
            Check(Pick(p["S2"], {2, 3}) == "A" && Pick(p["S2"], {2, 5}) == "A" && Pick(p["S2"], {2}) == "B" &&
                      Pick(p["S2"], {5}) == "A" && Pick(p["S2"], {}) == "B",
                "an inner card decides alone; the shared card from two places");
            Check(Pick(p["S3"], {5}) == "A" && Pick(p["S3"], {}) == "B", "a start into a shared card");
            Check(p["S2"].flow.predicates.size() == 3, "a shared card is copied once per program");
        }
        // Space and ground (0.12.299): measured where the actor stands, for no particular action. A card checking room
        // is a pair's selecting action only when it plays one.
        {
            const auto p = Parse("<Start id='S' key='G'><Row to='P'><Check kind='space' distance='100' "
                                 "direction='selfForward'/></Row><False to='A'/></Start><Pair id='P' clip='p.hkx'/>"
                                 "<Action id='A' clip='a.hkx'/>");
            Check(p.ok && p["S"].paired.enabled, "a card playing a pair is a pair's selecting action (T6)");
            const auto q = Parse("<Start id='S' key='G'><Row to='A'><Check kind='space' distance='100' "
                                 "direction='selfForward'/></Row></Start><Action id='A' clip='a.hkx'/>");
            Check(q.ok && !q["S"].paired.enabled, "room before an ordinary action needs no pair");
            Check(Parse("<Start id='S' key='G'><Check kind='space' distance='100' direction='selfForward'/>"
                        "<Row to='A'>" +
                        G(1) + "</Row><Row to='B'>" + G(2) + "</Row></Start>" + kABC)
                      .ok,
                "common conditions before rows need no action to measure for");
        }
        // A start choosing among ordinary actions takes the actor ahead (range, angle) or the combat target, key or
        // event; a pair's start and an HKX's own start keep their rules (0.12.299).
        {
            const std::string card = "<Row to='A'>" + G(1) + "</Row><False to='B'/></Start>" + kABC;
            auto p = Parse("<Start id='S' key='G'>" + card);
            Check(p.ok && p["S"].trigger.frontTarget && !p["S"].trigger.combatTarget &&
                      p["S"].trigger.targetRange == 200.0f && p["S"].trigger.targetAngle == 90.0f,
                "the actor ahead by default");
            p = Parse("<Start id='S' key='G' range='350' angle='60'>" + card);
            Check(p.ok && p["S"].trigger.targetRange == 350.0f && p["S"].trigger.targetAngle == 60.0f, "its cone");
            p = Parse("<Start id='S' key='G' target='combatTarget'>" + card);
            Check(p.ok && p["S"].trigger.combatTarget && !p["S"].trigger.frontTarget, "a key start's combat target");
            p = Parse("<Start id='S' event='Go' target='combatTarget'>" + card);
            Check(p.ok && p["S"].trigger.combatTarget, "an event start's combat target");
            Check(Refused(Parse("<Start id='S' key='G' target='combatTarget' range='300'>" + card), "range"),
                "no cone with the combat target");
            Check(Refused(Parse("<Start id='S' key='G' range='0'>" + card), "range"), "a cone has a size");
            Check(Refused(Parse(std::string("<Start to='A' key='G' range='300'/>") + kABC), "range"),
                "an HKX's own start has none");
            Check(Refused(Parse(std::string("<Start to='A' key='G' target='combatTarget'/>") + kABC), "target"),
                "nor a target");
            p = Parse("<Start id='S' event='Go'><Row to='P'>" + G(1) +
                      "</Row></Start><Pair id='P' clip='p.hkx'/><Action id='A' clip='a.hkx'/>");
            Check(p.ok && !p["S"].trigger.frontTarget, "a pair's start finds its partner by the pair");
        }
        // The card's shape is checked, each refusal naming what to write.
        {
            const std::pair<std::string, const char*> bad[] = {
                {"<Start key='G'><Row to='A'/></Start>", "<True>"},
                {"<Start key='G'><True to='A'/><Row to='B'>" + G(1) + "</Row></Start>", "True"},
                {"<Start key='G'><False to='A'/>" + G(1) + "</Start>", "False"},
                {"<Start key='G'><Row to='A'>" + G(1) + "</Row>" + G(2) + "</Start>", "공통 조건"},
                {"<Start key='G'><Row to='A' condition='X'>" + G(1) + "</Row></Start>", "하나만"},
                {"<Start key='G'><Row>" + G(1) + "<Condition id='X'><True to='A'/></Condition></Row></Start>", "id"},
                {"<Start key='G' condition='Nowhere'/>", "Nowhere"},
                {"<Start key='G' to='A'><Row to='B'>" + G(1) + "</Row></Start>", "공통 조건"},
                {"<Start key='G' to='A' condition='X'/>", "하나만"},
                {"<Start key='G'/>", "to"},
                {"<Start key='G'><Test kind='alive'/></Start>", "지원하지 않는 요소"},
            };
            for (const auto& [body, hint] : bad)
                Check(Refused(Parse(body + kABC), hint), (body + " refused naming " + hint).c_str());
            Check(Refused(Parse("<Condition id='X'><True condition='Y'/></Condition><Condition id='Y'><True "
                                "condition='X'/></Condition><Start key='G' condition='X'/>" +
                                std::string(kABC)),
                      "순환"),
                "shared cards that go round are refused");
            Check(Refused(Parse("<Condition id='X'><True to='A'/></Condition><Condition id='x'><True to='B'/>"
                                "</Condition>" +
                                std::string(kABC)),
                      "중복"),
                "shared card ids are unique");
        }
        // Starts (T1-T4, T7): the HKX's own, or a selecting action of its own.
        {
            const auto p =
                Parse("<Start key='G' to='A' cooldown='0.5' block='blockStart'/><Action id='A' clip='a.hkx'/>");
            Check(p.ok && p.actions.size() == 1 && p["A"].trigger.key == 0x22 &&
                      Near(p["A"].condition.cooldown, 0.5f) && p["A"].suppress.when == SuppressSpec::When::kStartable,
                "a start is its HKX's trigger, cooldown and block (T1)");
            const auto two = Parse("<Start key='G' to='A'/><Start key='H' to='A'/><Action id='A' clip='a.hkx'/>");
            Check(two.ok && two.actions.size() == 2 && two["A"].trigger.key == 0x22 &&
                      Pick(two["A_Start"], {}) == "A" && two["A_Start"].trigger.key == 0x23,
                "a second start to the same HKX is a selecting action (T3)");
            const auto held = Parse("<Start key='G' to='A' cooldown='1'/><Action id='A' clip='a.hkx'/>"
                                    "<Action id='B' clip='b.hkx'><Transition to='A' key='E'/></Action>");
            Check(held.ok && held["A"].trigger.key == 0 && Near(held["A"].condition.cooldown, 0) &&
                      Near(held["A_Start"].condition.cooldown, 1) && held["A_Start"].condition.inAir == 1,
                "a held start into an HKX reached otherwise keeps its cooldown apart (T2)");
            const auto named = Parse("<Start key='G'>" + G(1) + "<True to='A'/></Start>" + kABC);
            Check(named.ok && named.find("Start_G") && named.actions[0].id == "Start_G",
                "an unnamed card start is named by its trigger, at its place (T4, T7)");
        }
        // Game controls (0.12.292): control="jump" instead of a key, on a start, a transition, an input and a carry.
        {
            const auto p = Parse("<Start control='jump' to='A'/><Action id='A' clip='a.hkx'>"
                                 "<Transition to='B' control='sprint'/></Action><Action id='B' clip='b.hkx'/>");
            Check(p.ok, p.error.c_str());
            Check(
                p["A"].trigger.control == GameControl::kJump && p["A"].trigger.key == 0 && p["A"].trigger.gamepad == 0,
                "a start on a game control has no key");
            const auto toB =
                std::find_if(p["A"].links.begin(), p["A"].links.end(), [](const auto& l) { return l.action == "B"; });
            Check(toB != p["A"].links.end() && toB->control == GameControl::kSprint && toB->key == 0 && !toB->gamepad,
                "a transition on a game control");
            const auto named = Parse("<Inputs><Input id='leap' control='jump'/></Inputs><Start input='leap' to='A'/>"
                                     "<Action id='A' clip='a.hkx'/>");
            Check(named.ok && named["A"].trigger.control == GameControl::kJump, "a named input on a game control");
            const auto card = Parse("<Start control='readyWeapon'>" + G(1) + "<True to='A'/></Start>" + kABC);
            Check(card.ok && card.find("Start_readyWeapon"), "an unnamed card start is named by its control");
            Check(Refused(Parse("<Start control='fly' to='A'/><Action id='A' clip='a.hkx'/>"), "fly"),
                "an unknown control is refused, naming it");
            Check(Refused(Parse("<Start control='jump' key='G' to='A'/><Action id='A' clip='a.hkx'/>"), "control"),
                "a control and a key together are refused");
            Check(Refused(Parse("<Inputs><Input id='leap' control='jump' key='G'/></Inputs>"
                                "<Start input='leap' to='A'/><Action id='A' clip='a.hkx'/>"),
                      "control"),
                "an input with a control and a key is refused");
            Check(Refused(Parse("<Action id='A' clip='a.hkx'><Transition to='B' control='jump'/>"
                                "<Transition to='B' control='jump'/></Action><Action id='B' clip='b.hkx'/>"),
                      "중복"),
                "two transitions on one control are ambiguous");
        }
        // Transitions and QTE results through a card (T5): one selecting action each, after the document's.
        {
            const auto p = Parse("<Condition id='Next'><Row to='B'>" + G(1) +
                                 "</Row><False to='C'/></Condition>"
                                 "<Action id='A' clip='a.hkx'><Transition key='E' from='0.2' until='end-0.1' "
                                 "buffer='false'>" +
                                 G(2) +
                                 "<True to='B'/></Transition><Transition key='F' condition='Next'/>"
                                 "<QTE id='Q' at='0.1' key='R'><Success condition='Next'/></QTE></Action>"
                                 "<Action id='B' clip='b.hkx'><Transition key='E' condition='Next'/></Action>"
                                 "<Action id='C' clip='c.hkx'/>");
            Check(p.ok, p.error.c_str());
            const auto& a = p["A"];
            Check(p.actions.size() == 5 && p.actions[3].SelectsOnly() && p.actions[4].SelectsOnly() &&
                      p.find("A_Transition") && p.find("Next_Flow"),
                "a card's selecting action per transition, a shared card's once, after the document's");
            const auto* key = &a.links[0];
            for (const auto& l : a.links)
                if (l.key == 0x12) key = &l;
            Check(key->action == "A_Transition" && !key->buffered && Near(key->openAt, 0.2f) &&
                      key->closeRelativeToEnd && Near(key->closeAt, 0.1f),
                "a transition's window is its attributes");
            unsigned shared = 0;
            for (const auto* s : {&a, &p["B"]})
                for (const auto& l : s->links) shared += l.action == "Next_Flow";
            Check(shared == 3, "the shared card's transitions and the QTE result reach one selecting action");
            Check(Pick(p["Next_Flow"], {1}) == "B" && Pick(p["Next_Flow"], {}) == "C", "it picks as written");
        }
        {
            const auto p = Parse("<EventRegistry><Event name='Down'><QTE id='Up'><Success>" + G(1) +
                                 "<True to='A'/><False to='B'/></Success></QTE></Event></EventRegistry>" + kABC);
            Check(
                p.ok && p.events[0].qtes[0].results[0].to == "QTE_Up_Success" && Pick(p["QTE_Up_Success"], {1}) == "A",
                "an event QTE's result through a card starts the action it picks");
        }
        // HKX attributes (1.5) read into the fields the elements did.
        {
            const auto p = Parse("<Action id='A' clip='a.hkx' air='both' attack='AttackPower' enter='0.2' "
                                 "block='jumpStart' safety='2'><Exit blend='0.3'/><IFrame until='0.8'/>"
                                 "<MoveCancel at='1.2' blend='0.35'/><Vanilla to='Attack,event:Bash' from='0.3'/>"
                                 "<Vanilla id='late' to='Block' until='end-0.2' buffer='false'/>"
                                 "<Do at='0.1' op='hide'/><Camera/></Action>"
                                 "<Action id='B' clip='b.hkx' air='only'/>");
            Check(p.ok, p.error.c_str());
            const auto& a = p["A"];
            Check(a.condition.inAir == 1 && p["B"].condition.inAir == 2 && a.attack.enabled &&
                      a.attack.data == "AttackPower" && Near(a.enterBlend, 0.2f) && Near(a.safetyDuration, 2),
                "air, attack, enter and safety");
            Check(a.suppress.when == SuppressSpec::When::kActive && a.suppress.events.size() == 1,
                "an HKX's block is while it plays");
            Check(a.recovery.enabled && Near(a.recovery.at, 1.2f) && Near(a.exitBlend, 0.3f), "MoveCancel and Exit");
            Check(a.cancels.size() == 2 && a.cancels[0].id == "cancel1" && a.cancels[0].vanillaMatches.size() == 2 &&
                      Near(a.cancels[0].openAt, 0.3f) && a.cancels[1].id == "late" && a.cancels[1].mode == 1,
                "vanilla transitions and their windows");
            Check(a.pairFeatures.enabled && a.pairFeatures.timed.size() == 1, "Do and Camera sit in the action");
            const auto both = Parse("<Start key='G' to='A' block='jumpStart'/><Action id='A' clip='a.hkx' "
                                    "block='jumpStart'/>");
            Check(both.ok && both["A"].suppress.when == SuppressSpec::When::kBoth, "the same block both ways");
            Check(Refused(Parse("<Start key='G' to='A' block='sprintStart'/><Action id='A' clip='a.hkx' "
                                "block='jumpStart'/>"),
                      "같은 이벤트"),
                "different blocks on one HKX are refused");
            const auto pair = Parse("<Pair id='P' clip='p.hkx' lostBlend='0.6' reorient='false' interruptOnHit='true'>"
                                    "<FindPartner range='250'/><Do at='0.1' op='hide' target='receiver'/></Pair>");
            Check(pair.ok && Near(pair["P"].paired.lossBlend, 0.6f) && !pair["P"].paired.reorientSupport &&
                      pair["P"].pairFeatures.interruptOnThirdPartyHit && Near(pair["P"].paired.range, 250) &&
                      Near(pair["P"].paired.angle, 60) && Near(pair["P"].safetyDuration, 30),
                "a pair's attributes and FindPartner");
            Check(Refused(Parse("<Action id='A' clip='a.hkx' lostBlend='0.5'/>"), "페어 전용"),
                "pair attributes stay a pair's");
        }
        // Only format 6 is read; an element or attribute it does not have is refused by name.
        {
            Check(Refused(Parse("<Action id='A'/>"), "clip"), "an action names its clip");
            Check(Refused(Parse("<Action id='A' clip='a.hkx'><Require air='allow'/></Action>"), "Require"),
                "an element format 6 does not have");
            Check(Refused(Parse("<Action id='A' clip='a.hkx' fallback='1'/>"), "fallback"),
                "an attribute format 6 does not have");
            Check(Refused(Parse("<Action id='A' clip='a.hkx'/>", "5"), "형식 6"), "only format 6 is read");
        }
        std::cout << "Format 6 checks passed: " << checks << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        return 1;
    }
}
