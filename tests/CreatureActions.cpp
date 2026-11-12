// A creature's own actions (11-05): <Action rig="..."> plays in that creature's graph, its clip read from the
// creature's actor folder. The rules the reader holds, and where the clip catalog sends the files.
#include "Settings.h"
#include "declarations/XmlV2.h"
#include "runtime/DeclarationCatalog.h"
#include <algorithm>
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
            if (a.localId == id) return a;
        throw std::runtime_error(std::string("no action ") + id + (ok ? "" : ": " + error));
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
bool Refused(const Parsed& p, const char* hint) { return !p.ok && p.error.find(hint) != std::string::npos; }
bool Has(const std::vector<std::string>& list, const char* file) {
    return std::find(list.begin(), list.end(), file) != list.end();
}
}

int main() {
    try {
        // A creature action: its rig, its clip as written, and the parts every actor has.
        {
            const auto p =
                Parse("<Start event='attackStart' actor='npc' to='Howl'/>"
                      "<Action id='Howl' rig='wolf' clip='animations\\WolfHowl\\howl.hkx' enter='0.2' loop='true'>"
                      "<IFrame from='0' until='end'/><Transition to='Bite' event='biteNow'/></Action>"
                      "<Action id='Bite' rig='wolf' clip='animations\\WolfHowl\\bite.hkx'/>"
                      "<Action id='Slash' clip='Animations\\Slash.hkx'/>");
            Check(p.ok, "a creature action with its features reads");
            Check(p["Howl"].rig == PairedRig::Kind::kWolf && p["Bite"].rig == PairedRig::Kind::kWolf,
                "rig names the creature");
            Check(p["Slash"].rig == PairedRig::Kind::kCharacter, "an action without rig is the character's");
            Check(p["Howl"].clips[0].file == "animations\\WolfHowl\\howl.hkx", "the clip stays as written");

            // The catalog: the wolf's clips go to the wolf's project only, as written; the character's to the
            // character's only. Draugr and skeleton share a folder and so share their clips.
            std::vector<ActionSpec> specs = p.actions;
            const auto draugr = Parse("<Action id='Rise' rig='draugr' clip='animations\\Rise.hkx'/>");
            Check(draugr.ok, "a draugr action reads");
            specs.insert(specs.end(), draugr.actions.begin(), draugr.actions.end());
            DeclarationCatalog catalog;
            catalog.Build(specs);
            const auto& wolf = catalog.ForRig(PairedRig::Kind::kWolf);
            const auto& human = catalog.ForRig(PairedRig::Kind::kCharacter);
            Check(Has(wolf, "animations\\WolfHowl\\howl.hkx") && Has(wolf, "animations\\WolfHowl\\bite.hkx"),
                "the wolf's project gets its clips as written");
            Check(!Has(wolf, "Animations\\Slash.hkx"), "the wolf's project gets no character clip");
            Check(Has(human, "Animations\\Slash.hkx") && !Has(human, "animations\\WolfHowl\\howl.hkx") &&
                      !Has(catalog.files, "animations\\WolfHowl\\howl.hkx"),
                "the character's projects never bind a creature's clip");
            Check(Has(catalog.ForRig(PairedRig::Kind::kSkeleton), "animations\\Rise.hkx") &&
                      Has(catalog.ForRig(PairedRig::Kind::kDraugr), "animations\\Rise.hkx"),
                "draugr and skeleton share their clips");
            Check(!Has(catalog.ForRig(PairedRig::Kind::kBear), "animations\\Rise.hkx"), "other creatures do not");
        }
        // The rig names a known skeleton.
        Check(Refused(Parse("<Action id='A' rig='unicorn' clip='a.hkx'/>"), "unicorn"), "an unknown rig");
        // A creature-started pair (11-05 phase 2): <Pair rig> is its initiator, its clip stays relative to the
        // character's folder, and both roles' projects bind it.
        {
            const auto p = Parse("<Start event='NPCWolfAttackA' actor='npc' target='combatTarget' to='Pounce'/>"
                                 "<Pair id='Pounce' rig='wolf' clip='animations\\AITest\\WolfPounce.hkx'>"
                                 "<FindPartner rig='character' range='200' angle='90'/><Exit blend='0.3'/>"
                                 "<Transition to='Shake' event='shakeNow'/></Pair>"
                                 "<Action id='Shake' rig='wolf' clip='animations\\dog_spice_shake.hkx'/>"
                                 "<Pair id='Throw' clip='Animations\\Throw.hkx'/>");
            Check(p.ok, "a wolf's pair on a character reads");
            const auto& pounce = p["Pounce"];
            Check(pounce.rig == PairedRig::Kind::kWolf && pounce.paired.targetRig == PairedRig::Kind::kCharacter,
                "the pair's rig is its initiator, FindPartner rig its partner");
            Check(pounce.clips.size() == 2 && pounce.clips[1].file == "animations\\AITest\\WolfPounce.hkx",
                "the paired clip stays as written");
            Check(p["Throw"].rig == PairedRig::Kind::kCharacter, "a pair without rig is started by the character");
            DeclarationCatalog catalog;
            catalog.Build(p.actions);
            const auto pair = std::string("animations\\AITest\\WolfPounce.hkx");
            Check(Has(catalog.ForRig(PairedRig::Kind::kCharacter), pair.c_str()),
                "the character's projects bind the pair it partners");
            Check(
                Has(catalog.ForRig(PairedRig::Kind::kWolf), PairedRig::ClipPath(pair, PairedRig::Kind::kWolf).c_str()),
                "the wolf's project binds it through the character's folder");
            Check(!Has(catalog.ForRig(PairedRig::Kind::kWolf), pair.c_str()),
                "never as written (that would be the wolf's own folder)");
            Check(
                !Has(catalog.ForRig(PairedRig::Kind::kBear), PairedRig::ClipPath(pair, PairedRig::Kind::kBear).c_str()),
                "another creature's does not");
        }
        // What a creature-started pair may not have.
        {
            Check(
                Refused(Parse("<Pair id='P' rig='wolf' clip='p.hkx'><FindPartner rig='bear'/></Pair>"), "상대는 사람"),
                "a creature's pair partners a character only");
            Check(Refused(Parse("<Pair id='P' rig='wolf' clip='p.hkx'><Carry target='physics'/></Pair>"), "운반"),
                "a creature does not carry");
            Check(Refused(Parse("<Pair id='P' rig='wolf' clip='p.hkx'><Camera reset='x'/></Pair>"),
                      "사람(character) 동작에만"),
                "a camera on a creature's pair");
            Check(Refused(Parse("<Pair id='P' rig='wolf' clip='p.hkx'><Transition to='B' event='e'/></Pair>"
                                "<Action id='B' clip='b.hkx'/>"),
                      "다른 스켈레톤"),
                "a wolf's pair into a character action");
            Check(Refused(
                      Parse("<Pair id='P' rig='wolf' clip='p.hkx'><Transition to='A' partnerTo='B' event='e'/>"
                            "</Pair><Action id='A' rig='wolf' clip='a.hkx'/><Action id='B' rig='wolf' clip='b.hkx'/>"),
                      "partnerTo는 상대"),
                "the partner's next action is the partner's rig");
            Check(Parse("<Pair id='P' rig='wolf' clip='p.hkx'><Transition to='A' partnerTo='B' event='e'/></Pair>"
                        "<Action id='A' rig='wolf' clip='a.hkx'/><Action id='B' clip='b.hkx'/>")
                      .ok,
                "the initiator goes on as a wolf, the partner as a character");
            Check(Parse("<Action id='A' rig='wolf' clip='a.hkx'><Transition to='B' event='e'/></Action>"
                        "<Action id='B' rig='dog' clip='b.hkx'/>")
                      .ok,
                "a transition within one skeleton family (wolf to dog)");
        }
        // What only a character action has.
        {
            for (const char* attribute : {"view='first'", "otherView='switch'", "air='both'", "attack='x'"})
                Check(Refused(Parse(std::string("<Action id='A' rig='bear' clip='a.hkx' ") + attribute + "/>"),
                          "사람(character) 동작에만"),
                    "a character-only attribute on a creature action");
            Check(Refused(Parse("<Action id='A' rig='bear' clip='a.hkx'><Camera reset='x'/></Action>"),
                      "사람(character) 동작에만"),
                "a camera on a creature action");
            Check(Refused(Parse("<Action id='A' rig='bear' clip='a.hkx'><Do at='0.1' op='hide'/></Action>"),
                      "사람(character) 동작에만"),
                "hiding weapons on a creature action");
        }
        // A transition stays within one skeleton, either way.
        {
            Check(Refused(Parse("<Action id='A' rig='wolf' clip='a.hkx'><Transition to='B' event='e'/></Action>"
                                "<Action id='B' clip='b.hkx'/>"),
                      "다른 스켈레톤"),
                "a wolf action into a character action");
            Check(Refused(Parse("<Action id='A' clip='a.hkx'><Transition to='B' event='e'/></Action>"
                                "<Action id='B' rig='wolf' clip='b.hkx'/>"),
                      "다른 스켈레톤"),
                "a character action into a wolf action");
        }
        std::cout << "Creature action checks passed: " << checks << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        return 1;
    }
}
