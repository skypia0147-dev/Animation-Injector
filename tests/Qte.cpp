#include "Settings.h"
#include "declarations/XmlV2.h"
#include "runtime/QteSpec.h"
#include "runtime/DeclarationCatalog.h"
#include <iostream>
#include <stdexcept>
using namespace AnimationInjector;
unsigned checks = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
bool Parse(const std::string& content, std::vector<ActionSpec>& out, bool pair = false) {
    pugi::xml_document doc;
    const std::string tag = pair ? "Pair" : "Action";
    doc.load_string(("<AnimationSet version='4' id='QteTest'><" + tag + " id='A' clip='a.hkx'>" + content + "</" + tag +
                     "><Action id='B' clip='b.hkx'/><Action id='C' clip='c.hkx'/></AnimationSet>")
            .c_str());
    std::string mod, error;
    out.clear();
    // The plugin's own resolver: unknown names return UINT32_MAX, not 0.
    return XmlV2::Compile(doc, "test", out, mod, error, Declarations::ResolveKeyName);
}
int main(int argc, char** argv) {
    try {
        QteSpec q;
        QteClock clock;
        clock.Advance(QteClock::target * 2.4f, q);
        clock.Press(q);
        Check(clock.outcome == QteOutcome::Perfect, "center perfect");
        clock.Press(q);
        clock.Resolve(QteOutcome::Failure);
        Check(clock.outcome == QteOutcome::Perfect, "result exactly once");
        clock = {};
        clock.Advance((QteClock::target + .04f) * 2.4f, q);
        clock.Press(q);
        Check(clock.outcome == QteOutcome::Success, "success band");
        clock = {};
        clock.Press(q);
        Check(clock.outcome == QteOutcome::Failure, "early press fails");
        clock = {};
        clock.Advance(2.4f, q);
        clock.Press(q);
        Check(clock.outcome == QteOutcome::Failure, "timeout beats input");
        clock = {};
        clock.Advance(0, q);
        Check(clock.elapsed == 0, "pause freezes");
        clock.Advance(-1, q);
        Check(clock.elapsed == 0, "negative delta ignored");
        q.speed = 2;
        clock.Advance(QteClock::target * 1.2f, q);
        clock.Press(q);
        Check(clock.outcome == QteOutcome::Perfect, "speed scales input duration");
        std::vector<ActionSpec> out;
        for (const auto name : qteAnchorNames) {
            const auto xml = "<QTE id='Anchor' at='0' anchor='" + std::string(name) + "' x='0.1' y='0.15'/>";
            Check(Parse(xml, out), "anchor accepted by XML normalizer");
        }
        Check(Parse("<QTE id='Anchor' at='0' anchor='center' x='-0.1' y='-0.2'/>", out),
            "center negative offsets accepted");
        Check(!Parse("<QTE id='Anchor' at='0' anchor='invalid'/>", out), "unknown anchor rejected");
        Check(!Parse("<QTE id='Anchor' at='0' anchor='center' x='-1.1'/>", out), "invalid anchor offset rejected");

        for (auto type : {QteType::TimingBar, QteType::RotatingNeedle}) {
            q = QteSpec{};
            q.type = type;
            clock = {};
            clock.Advance(QteClock::target * 2.4f, q);
            clock.Press(q);
            Check(clock.outcome == QteOutcome::Perfect, "timing skins use the same exact target");
        }
        q = QteSpec{};
        q.type = QteType::HoldRelease;
        clock = {};
        clock.Press(q);
        Check(clock.holding && clock.outcome == QteOutcome::None, "hold down waits for release");
        clock.Advance(QteClock::target * 2.4f, q);
        clock.Release(q);
        Check(clock.outcome == QteOutcome::Perfect && !clock.holding, "release in target succeeds");
        clock = {};
        clock.Release(q);
        Check(clock.outcome == QteOutcome::None, "unpaired release ignored");
        clock.Press(q);
        clock.Advance(.1f, q);
        clock.Release(q);
        Check(clock.outcome == QteOutcome::Failure, "early hold release fails");
        clock = {};
        clock.Advance(1, q);
        clock.Press(q);
        clock.Advance(1.4f, q);
        clock.Release(q);
        Check(clock.outcome == QteOutcome::Failure, "late hold cannot extend total deadline");
        q.type = QteType::RapidPress;
        q.pressCount = 3;
        clock = {};
        clock.Press(q);
        clock.Press(q);
        Check(clock.completed == 2 && clock.outcome == QteOutcome::None, "rapid counts partial presses");
        clock.Press(q);
        Check(clock.outcome == QteOutcome::Perfect, "rapid early completion perfect");
        clock = {};
        clock.Advance(1.5f, q);
        for (int i = 0; i < 3; ++i) clock.Press(q);
        Check(clock.outcome == QteOutcome::Success, "rapid late completion success");
        clock = {};
        q.perfectBefore = 0;
        for (int i = 0; i < 3; ++i) clock.Press(q);
        Check(clock.outcome == QteOutcome::Success, "perfect completion can be disabled");
        q.type = QteType::KeySequence;
        q.sequenceCount = 4;
        clock = {};
        clock.Press(q, true);
        clock.Press(q, false);
        Check(clock.outcome == QteOutcome::Failure && clock.completed == 1, "wrong sequence input fails");
        clock = {};
        for (int i = 0; i < 4; ++i) clock.Press(q, true);
        Check(clock.outcome == QteOutcome::Success, "sequence completes exactly once");
        clock.Press(q);
        Check(clock.completed == 4, "completed sequence immutable");
        for (const char* type : {"splitArc", "timingBar", "rotatingNeedle", "holdRelease", "rapidPress"})
            Check(
                Parse(std::string("<QTE id='Q' at='0' type='") + type + "' pressCount='12' perfectBefore='.4'/>", out),
                "all QTE types parse");
        Check(Parse("<QTE id='Q' at='0' type='keySequence'><Input key='E'/><Input key='E'/></QTE>", out),
            "sequence repeated keys accepted");
        Check(out[0].qtes[0].sequence[0].key == 18, "sequence keys resolved");
        Check(
            Parse("<QTE id='Q' at='0' type='keySequence'><Input gamepad='A'/><Input gamepad='code:8192'/></QTE>", out),
            "gamepad-only sequence accepted");
        Check(out[0].qtes[0].sequence[0].gamepad == 0x1000 && out[0].qtes[0].sequence[1].gamepad == 0x2000,
            "pad buttons by name or code");
        Check(!Parse("<QTE id='Q' at='0' gamepad='4096'/>", out), "a bare pad number is refused (format 4)");
        for (const char* invalid : {"<QTE id='Q' at='0' type='wrong'/>", "<QTE id='Q' at='0' pressCount='1.5'/>",
                 "<QTE id='Q' at='0' pressCount='0'/>", "<QTE id='Q' at='0' perfectBefore='1.1'/>",
                 "<QTE id='Q' at='0' type='keySequence'/>",
                 "<QTE id='Q' at='0' type='keySequence'><Input key='E'/></QTE>",
                 "<QTE id='Q' at='0'><Input key='E'/></QTE>",
                 "<QTE id='Q' at='0' type='keySequence'><Input key='E'/><Input gamepad='A'/></QTE>",
                 "<QTE id='Q' at='0' gamepad='1.5'/>"})
            Check(!Parse(invalid, out), "invalid type-specific QTE rejected");
        Check(Parse("<QTE id='Escape' at='event:BeginQTE' key='E'><Success event='Won'/><Failure to='B'/></QTE>", out),
            "event and transition routes parse");
        Check(out[0].qtes[0].event == "BeginQTE", "annotation binding preserved");
        Check(out[0].links.size() == 1 && out[0].links[0].event == "AI_A_QTE_Escape_Failure" &&
                  out[0].links[0].action == "B" && out[0].qtes[0].results[1].event == "AI_A_QTE_Escape_Failure",
            "route gets unique event");
        Check(
            Parse(
                "<QTE id='Escape' at='0.3' key='E' inputOwner='partner' eventSource='both'><Success to='B' partnerTo='C'/></QTE>",
                out, true),
            "pair handoff and victim input");
        {  // A hidden QTE (0.12.266): read, a boolean like reducedMotion.
            Check(Parse("<QTE id='Q' at='0' hidden='true' type='rapidPress' pressCount='5'/>", out) &&
                      out[0].qtes[0].hidden,
                "hidden QTE read");
            Check(Parse("<QTE id='Q' at='0'/>", out) && !out[0].qtes[0].hidden, "shown by default");
            Check(!Parse("<QTE id='Q' at='0' hidden='yes'/>", out), "hidden is a boolean");
        }
        {  // Unset, the input owner follows the event's sender (0.12.263).
            const auto owner = [&](const char* xml, bool pair) {
                return Parse(xml, out, pair) ? out[0].qtes[0].inputOwner : std::string("refused");
            };
            Check(owner("<QTE id='Q' at='event:S' eventSource='partner'/>", true) == "partner",
                "a victim's event is the victim's QTE");
            Check(owner("<QTE id='Q' at='event:S'/>", true) == "self", "an attacker's event is the attacker's QTE");
            Check(owner("<QTE id='Q' at='event:S' eventSource='both'/>", true) == "player",
                "an event from both is the player's in any role");
            Check(owner("<QTE id='Q' at='0.3'/>", true) == "player", "a timed pair QTE is the player's in any role");
            Check(owner("<QTE id='Q' at='event:S' eventSource='partner' inputOwner='player'/>", true) == "player",
                "a set input owner wins");
            Check(owner("<QTE id='Q' at='event:S'/>", false) == "player", "a single action's QTE is the player's");
        }
        for (const char* invalid : {"<QTE id='Q' at='0' speed='0'/>", "<QTE id='Q' at='0' perfectWindow='.2'/>",
                 "<QTE id='Q' at='end'/>", "<QTE id='Q' at='0' key='Invalid'/>",
                 "<QTE id='Q' at='0' inputOwner='partner'/>", "<QTE id='Q' at='0'><Success to='Missing'/></QTE>",
                 "<QTE id='Q' at='0'><Success event='E' to='B'/></QTE>", "<QTE id='Q' at='0'/><QTE id='Q' at='1'/>",
                 "<QTE id='Q' at='0'><Success to='B' partnerTo='C'/></QTE>", "<QTE id='Q' at='NaN'/>",
                 "<QTE id='Q' at='0' x='2'/>"})
            Check(!Parse(invalid, out), "invalid QTE rejected");
        Check(
            Parse(
                "<QTE id='Q' at='event:Start' control='rightAttack' stopEvent='Stop' replaceActive='true' feedback='.3' group='Grapple QTE' speed='4'><Success soundPlugin='FH_Grapple.esp' soundForm='822'/><Failure event='Failed' soundPlugin='FH_Grapple.esp' soundForm='824'/></QTE>",
                out),
            "attack control, stop, feedback, group and sounds parse");
        {  // Keys that will be read must resolve; keys replaced by control or a sequence are ignored.
            const auto key = [&] { return std::to_string(out[0].qtes[0].key); };
            for (const char* invalid : {"<QTE id='Q' at='0' key='Ee'/>", "<QTE id='Q' at='0' key='0x12'/>",
                     "<QTE id='Q' at='0' key='code:266'/>",
                     "<QTE id='Q' at='0' type='keySequence'><Input key='E'/><Input key='Ee'/></QTE>",
                     "<QTE id='Q' at='0' key=''/>"})
                if (Parse(invalid, out)) {
                    std::cerr << "accepted: " << invalid << '\n';
                    Check(false, "unknown or missing QTE key rejected");
                } else
                    ++checks;
            Check(Parse("<QTE id='Q' at='0' key='code:18'/>", out) && std::string_view(key()) == "18",
                "code notation accepted");
            Check(Parse("<QTE id='Q' at='0' key='Mouse1'/>", out) && std::string_view(key()) == "256",
                "mouse name resolves");
            Check(Parse("<QTE id='Q' at='0' key='' gamepad='A'/>", out) && std::string_view(key()) == "0",
                "gamepad-only QTE keeps no key");
            Check(Parse("<QTE id='Q' at='0' key='LMB' control='rightAttack'/>", out) && std::string_view(key()) == "0",
                "control replaces an unused key name");
            Check(Parse("<QTE id='Q' at='0' key='LMB' type='keySequence'><Input key='E'/><Input key='Q'/></QTE>", out),
                "sequence ignores the single key");
        }
        for (const char* invalid : {"<QTE id='Q' at='0' control='other'/>",
                 "<QTE id='Q' at='0' control='rightAttack' type='keySequence'><Input key='E'/><Input key='E'/></QTE>",
                 "<QTE id='Q' at='0' stopEvent='has space'/>", "<QTE id='Q' at='0' feedback='0'/>",
                 "<QTE id='Q' at='0' replaceActive='yes'/>", "<QTE id='Q' at='0'><Success soundPlugin='F.esp'/></QTE>",
                 "<QTE id='Q' at='0'><Success soundForm='822'/></QTE>",
                 "<QTE id='Q' at='0'><Success soundPlugin='F.esp' soundForm='nope'/></QTE>",
                 "<QTE id='Q' at='0' channel='4'/>", "<QTE id='Q' at='0' channel='1.5'/>",
                 "<QTE id='Q' at='0' channel='-1'/>"})
            Check(!Parse(invalid, out), "invalid native QTE extensions rejected");
        Check(Parse("<QTE id='Q' at='0' channel='3'/>", out) && out[0].qtes[0].channel == 3,
            "a channel passes to the runtime declaration");
        ActionSpec catalogSpec;
        QteSpec catalogQte;
        catalogQte.event = "Begin";
        catalogQte.stopEvent = "Stop";
        catalogSpec.qtes.push_back(catalogQte);
        DeclarationCatalog stopCatalog;
        stopCatalog.Build({catalogSpec});
        Check(std::find(stopCatalog.events.begin(), stopCatalog.events.end(), "Stop") != stopCatalog.events.end(),
            "stop event registered in injected behavior");
        if (argc > 1) {
            std::vector<ActionSpec> specs;
            Check(Declarations::LoadXmlFile(argv[1], specs) && specs.size() == 3,
                "editor fixture loads through production settings parser");
            Check(specs[0].qtes.size() == 1, "production settings retain QTE");
            const auto& s = specs[0].qtes[0];
            Check(s.key == 18 && std::fabs(s.at - .3f) < .0001f && std::fabs(s.speed - 1.5f) < .0001f,
                "production key/time/speed retained");
            Check(s.results[0].event == "QTE_Success" && s.results[0].recipient == "both",
                "result event and role retained");
            Check(specs[0].links.size() == 1 && s.results[1].event == specs[0].links[0].event &&
                      specs[0].links[0].partnerAction == "QteAction3",
                "both ordinary destinations use the existing handoff");
            DeclarationCatalog catalog;
            catalog.Build(specs);
            Check(std::find(catalog.events.begin(), catalog.events.end(), s.results[0].event) != catalog.events.end(),
                "result event is registered in injected graphs");
        }
        if (argc > 2) {
            std::vector<ActionSpec> specs;
            Check(Declarations::LoadXmlFile(argv[2], specs) && specs[0].qtes.size() == 6,
                "all six editor QTEs load in production parser");
            for (unsigned i = 0; i < 6; ++i)
                Check(static_cast<unsigned>(specs[0].qtes[i].type) == i, "type retained by production settings");
            Check(specs[0].qtes[4].pressCount == 7 && specs[0].qtes[4].perfectBefore == .4f, "rapid settings retained");
            const auto& seq = specs[0].qtes[5];
            Check(seq.sequenceCount == 4 && seq.sequence[1].key == 16 && seq.sequence[1].gamepad == 8192 &&
                      seq.sequence[3].key == 19,
                "ordered keyboard and pad sequence retained");
        }
        if (argc > 3) {
            for (unsigned i = 1; i < qteAnchorNames.size(); ++i) {
                std::vector<ActionSpec> specs;
                const auto path = std::string(argv[3]) + "/QteAnchor-" + std::string(qteAnchorNames[i]) + ".xml";
                Check(Declarations::LoadXmlFile(path.c_str(), specs) && specs.size() == 3 && specs[0].qtes.size() == 6,
                    "anchor fixture parses");
                for (const auto& q : specs[0].qtes)
                    Check(q.anchor == static_cast<QteAnchor>(i) && std::fabs(q.x - (i == 1 ? -.1f : .05f)) < .00001f &&
                              std::fabs(q.y - .1f) < .00001f && q.scale == 1.5f,
                        "production settings retain anchor and offsets");
            }
        }
        if (argc > 4) {
            std::vector<ActionSpec> specs;
            Check(Declarations::LoadXmlFile(argv[4], specs) && specs.size() == 21,
                "migrated Grapple production declarations load");
            unsigned count = 0;
            for (const auto& a : specs) {
                if (!a.paired.enabled) {
                    Check(a.qtes.empty(), "ordinary stage motions have no extra QTE");
                    continue;
                }
                Check(a.qtes.size() == 20, "all stage and role rules retained per pair");
                for (const auto& q : a.qtes) {
                    ++count;
                    const int stage = std::stoi(q.event.substr(q.event.size() - 2));
                    const auto suffix = q.inputOwner == "partner" ? "_PlayerTarget" : "";
                    const auto event = std::string("Grapple_QTE_Stage") +
                                       (stage <= 3      ? "01"
                                           : stage <= 6 ? "02"
                                                        : "10") +
                                       suffix;
                    Check(q.type == QteType::SplitArc && q.speed == 4 && q.control == "rightAttack" &&
                              q.replaceActive && q.feedback == .3f && q.delay == .3f,
                        "A4 common settings survive runtime loader");
                    Check(q.at < 0 && q.fallback < 0 && q.stopEvent == "Grapple_Stage_Stop" &&
                              q.inputOwner == q.eventSource,
                        "event-only player role and explicit stop survive");
                    Check(q.results[stage <= 3 ? 1 : 0].event == event &&
                              q.results[stage <= 3 ? 1 : 0].recipient == "self" &&
                              q.results[stage <= 3 ? 0 : 1].event.empty(),
                        "each result routes exactly once on original initiator");
                    Check(q.results[0].soundPlugin == "FH_Grapple.esp" && q.results[0].soundForm == "822" &&
                              q.results[1].soundForm == "824",
                        "existing sound descriptors retained");
                }
            }
            Check(count == 280, "all 280 Grapple rules verified");
        }
        std::cout << checks << " QTE checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
