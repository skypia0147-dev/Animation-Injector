// Part actions (body=, runtime/BodyPart.h, docs/설계-상체레이어.md): what the parser reads and what it refuses.
#include "Settings.h"
#include "declarations/XmlV2.h"
#include "runtime/BodyPart.h"
#include "runtime/EventRegistrySpec.h"
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
    const ActionSpec* Find(const std::string& id) const {
        for (const auto& a : actions)
            if (a.id == "Test_" + id || a.id == id) return &a;
        return nullptr;
    }
};
Parsed Parse(const std::string& body) {
    const std::string xml = "<AnimationSet version='6' id='Test'>" + body + "</AnimationSet>";
    pugi::xml_document doc;
    doc.load_string(xml.c_str());
    Parsed p;
    std::vector<EventRuleSpec> events;
    std::string mod;
    p.ok = XmlV2::Compile(doc, "test.xml", p.actions, events, mod, p.error, Declarations::ResolveKeyName);
    return p;
}
bool Rejected(const std::string& body, const char* says = nullptr) {
    const auto p = Parse(body);
    if (p.ok) return false;
    return !says || p.error.find(says) != std::string::npos;
}
const std::string kStart = "<Start key='G' to='Drink'/>";
std::string Part(const std::string& attrs, const std::string& children = {}) {
    return kStart + "<Action id='Drink' clip='Animations\\Drink.hkx' " + attrs + ">" + children + "</Action>";
}
}  // namespace

int main() {
    try {
        // The three parts, read; no body is the whole body.
        for (const auto& [text, part] : std::initializer_list<std::pair<const char*, BodyPart>>{
                 {"upper", BodyPart::kUpper}, {"rightArm", BodyPart::kRightArm}, {"leftArm", BodyPart::kLeftArm}}) {
            const auto p = Parse(Part(std::string("body='") + text + "'"));
            Check(p.ok, p.error.c_str());
            const auto* a = p.Find("Drink");
            Check(a && a->body == part && a->Part(), "body read");
            Check(a->view.view == View::kThird, "a part action plays in third person");
        }
        {
            const auto p = Parse(Part(""));
            Check(p.ok && p.Find("Drink") && !p.Find("Drink")->Part(), "no body: the whole body");
        }
        Check(LayerOf(BodyPart::kUpper) == 0 && LayerOf(BodyPart::kRightArm) == 1 && LayerOf(BodyPart::kLeftArm) == 2 &&
                  LayerOf(BodyPart::kWhole) == -1,
            "layers in stacking order");

        // A loop plays on until it is ended: maxHold only when written, or its default with a release transition.
        {
            const auto p = Parse(Part("body='upper' loop='true'"));
            Check(
                p.ok && p.Find("Drink")->loop.enabled && p.Find("Drink")->loop.maxHold == 0.0f, "loop without maxHold");
            const auto q = Parse(Part("body='upper' loop='true' maxHold='4'"));
            Check(q.ok && q.Find("Drink")->loop.maxHold == 4.0f, "loop with maxHold");
            const auto r = Parse(Part("body='upper' loop='true'", "<Transition to='Drink' on='release'/>"));
            Check(r.ok && r.Find("Drink")->loop.maxHold == 3.0f, "a charge hold keeps the default maxHold");
        }
        // What a part action may declare: enter blend, Exit by time, clip-time Do, transitions as a full body's.
        {
            const auto p = Parse(Part("body='rightArm' enter='0.2'",
                "<Exit at='end-0.2' blend='0.4'/><Do at='0.5' op='sound' file='sound\\fx\\a.wav'/>"
                "<Transition key='H' end='true'/><Transition key='J' end='true' on='release'/>"));
            Check(p.ok, p.error.c_str());
            const auto* a = p.Find("Drink");
            Check(a->enterBlend == 0.2f && a->exitBlend == 0.4f && a->pairFeatures.timed.size() == 1, "allowed parts");
            Check(a->links.size() == 2 && a->links[0].end && a->links[1].onRelease, "end transitions");
        }

        // Refused.
        Check(Rejected(Part("body='legs'"), "body는 upper"), "unknown part");
        Check(Rejected(kStart + "<Pair id='Drink' clip='Animations\\Drink.hkx' body='upper'/>"), "a pair");
        Check(Rejected(Part("body='upper' view='first'"), "3인칭"), "first person");
        Check(Rejected(Part("body='upper' view='both'"), "3인칭"), "both views");
        for (const char* attr : {"otherView='block'", "air='both'", "attack='attackStart'", "block='attackStart'"})
            Check(Rejected(Part(std::string("body='upper' ") + attr), "부위 동작"), "a state's attribute");
        for (const char* child : {"<Rotate/>", "<IFrame/>", "<HyperArmor/>", "<Cost stamina='5'/>",
                 "<TimeScale factor='0.5'/>", "<MoveCancel/>"})
            Check(Rejected(Part("body='upper'", child), "부위 동작"), "a state's feature");
        Check(Rejected(Part("body='upper'", "<Transition key='H' end='true' on='move'/>"), "이동 전환"), "on move");
        Check(Rejected(Part("body='upper'", "<Transition key='H' to='Other' blend='0.2'/>") +
                           "<Action id='Other' clip='Animations\\Other.hkx'/>",
                  "blend"),
            "a part transition's blend");
        Check(Rejected(Part("body='upper'", "<Exit at='DrinkDone'/>")), "an Exit on an event (no action has one)");

        // Transitions as a full body's (charge 11-20): a start and hold on the upper body, the release standing.
        {
            const auto p = Parse(
                "<Start key='G' press='hold' to='ChargeStart'/>"
                "<Action id='ChargeStart' clip='Animations\\CS.hkx' body='upper'><Transition to='ChargeHold' on='end'/>"
                "</Action>"
                "<Action id='ChargeHold' clip='Animations\\CH.hkx' body='upper' loop='true'>"
                "<Transition to='Release' on='release'/><Transition key='H' end='true'/></Action>"
                "<Action id='Release' clip='Animations\\R.hkx'/>");
            Check(p.ok, p.error.c_str());
            const auto* start = p.Find("ChargeStart");
            const auto* hold = p.Find("ChargeHold");
            Check(start && start->Part() && start->links.size() == 1 && start->links[0].onEnd, "a part on=\"end\"");
            Check(hold && hold->loop.maxHold == 3.0f && hold->links.size() == 2 && hold->links[0].onRelease &&
                      hold->links[1].end,
                "a part hold: release and end");
            Check(p.Find("Release") && !p.Find("Release")->Part(), "the release on the whole body");
        }
        // Keys, events, press kinds and cards from a part action; a full-body action to a part one.
        {
            const auto p =
                Parse(Part("body='rightArm'",
                          "<Transition key='H' press='hold' to='Other'/><Transition event='Wave' to='Other'/>"
                          "<Transition key='J'><True to='Swing'/></Transition>") +
                      "<Action id='Other' clip='Animations\\Other.hkx' body='leftArm'/>"
                      "<Action id='Swing' clip='Animations\\Swing.hkx'>"
                      "<Transition key='K' to='Drink'/></Action>");
            Check(p.ok, p.error.c_str());
            Check(p.Find("Drink")->links.size() == 3, "a part action's transitions");
            Check(p.Find("Swing")->links.size() == 1, "a full-body action to a part one");
        }
        // Not with a pair, either way.
        Check(Rejected(kStart + "<Action id='Drink' clip='Animations\\Drink.hkx' body='upper'>"
                                "<Transition key='H' to='Throw'/></Action>"
                                "<Pair id='Throw' clip='Animations\\Throw.hkx'/>",
                  "페어로는"),
            "a part action to a pair");
        Check(Rejected(kStart + "<Action id='Drink' clip='Animations\\Drink.hkx' body='upper'/>"
                                "<Start key='K' to='Throw'/><Pair id='Throw' clip='Animations\\Throw.hkx'>"
                                "<Transition key='H' to='Drink'/></Pair>",
                  "페어에서"),
            "a pair to a part action");
        std::cout << "BodyParts: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "BodyParts FAILED after " << checks << " checks: " << e.what() << "\n";
        return 1;
    }
}
