#include "runtime/AnnotationCommands.h"
#include "Settings.h"
#include "declarations/XmlV2.h"
#include "runtime/EventRegistryMerge.h"
#include "runtime/EventValuesMatch.h"
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
const char* kAction = "<Action id='A' clip='a.hkx'/>";
bool Rejected(const std::string& body, const char* version = "6") { return !Parse(body, version).ok; }
}

int main() {
    try {
        // The registry: names, functions, defaults and an event registered only for the graphs.
        {
            const auto p = Parse(std::string(kAction) + R"(<EventRegistry>
                <Event name="Grapple_Stom"><Do op="spell" plugin="FH_Grapple.esp" form="0x80B"/></Event>
                <Event name="Grapple_CamShake"><Do op="shake"/></Event>
                <Event name="MCO_WinOpen"/>
                <Event name="Hit"><Do op="hide"/><Do op="iframeOn"/></Event>
                <Event name="Fire"><Do op="fireArrow"/></Event>
            </EventRegistry>)");
            Check(p.ok, "a registry parses");
            Check(p.events.size() == 5 && p.actions.size() == 1, "every event and the action are read");
            const auto& stom = p.events[0];
            Check(stom.name == "Grapple_Stom" && stom.source == "test.xml" && stom.functions.size() == 1,
                "name, source and function");
            const auto& spell = stom.functions[0];
            Check(spell.op == EventOp::kSpell && spell.plugin == "FH_Grapple.esp" && spell.form == "80B" &&
                      spell.delay == 0.0f,
                "spell settings, form without 0x");
            Check(spell.target == EventTarget::kSender, "an event function changes its sender");
            const auto& shake = p.events[1].functions[0];
            Check(shake.op == EventOp::kShake && shake.target == EventTarget::kSender &&
                      std::fabs(shake.strength - 1.0f) < 1e-6f && std::fabs(shake.duration - 0.5f) < 1e-6f,
                "a registry shake holds the default numbers");
            Check(p.events[2].name == "MCO_WinOpen" && p.events[2].functions.empty(), "an event without functions");
            Check(p.events[3].functions.size() == 2 && p.events[3].functions[1].op == EventOp::kIFrameOn,
                "several functions");
            Check(p.events[4].functions[0].op == EventOp::kFireArrow, "fireArrow is an event function");
        }
        Check(Parse("<EventRegistry><Event name='OnlyRegistered'/></EventRegistry>").ok,
            "a file may hold only registrations");
        Check(Rejected("<EventRegistry/>") && Rejected(""), "a file needs an action or an event");
        for (const char* old : {"2", "3", "5"}) {
            const auto p = Parse(kAction, old);
            Check(!p.ok && p.error.find("형식 6") != std::string::npos, "only format 6 is read");
        }
        Check(Rejected("<EventRegistry><Event name='Same'/><Event name='same'/></EventRegistry>"),
            "one name twice in a file, ignoring case");
        Check(Rejected("<EventRegistry><Event name='X'><Do op='spell'/></Event></EventRegistry>"),
            "a spell needs its plugin and form");
        Check(
            Rejected("<EventRegistry><Event name='X'><Do op='hide' plugin='a.esp' form='1'/></Event></EventRegistry>"),
            "plugin and form only where a spell is used");
        // An event function changes its sender (0.12.273): it has no target or sender filter.
        for (const char* attr : {"target='player'", "target='sender'", "from='player'", "from='any'"}) {
            const auto p = Parse(
                std::string("<EventRegistry><Event name='X'><Do op='hide' ") + attr + "/></Event></EventRegistry>");
            Check(!p.ok, "no target or sender filter");
        }
        Check(Rejected("<Pair id='P' clip='p.hkx'><Do at='0.1' op='hide' target='lead'/></Pair>"),
            "lead/support are not role names (the engine's lead is the receiver)");
        Check(Rejected("<EventRegistry><Event name='X'><Do op='hide' at='1'/></Event></EventRegistry>"),
            "an event function has no clip time");
        Check(Rejected("<EventRegistry><Event name='a|b'/></EventRegistry>"), "event names are checked");

        // Removed forms: event effects, extra registrations and event moments of clip functions.
        Check(
            Rejected("<Pair id='P' clip='p.hkx'><Effect name='X' op='hide'/></Pair>"), "pair Effect elements are gone");
        Check(Rejected("<Pair id='P' clip='p.hkx'><Effect at='1' op='hide'/></Pair>"),
            "including clip-time Effect elements");
        Check(Rejected("<Action id='A' clip='a.hkx'><Events extra='X'/></Action>"), "Events extra is gone");
        Check(Rejected("<Action id='A' clip='a.hkx'><FireArrow at='event:Fire'/></Action>"),
            "an event shot is a registry function (its numbers the action's EventValues)");
        Check(Rejected("<Action id='A' clip='a.hkx'><IFrame from='event:On' until='end'/></Action>"),
            "windows open at clip times");
        Check(Rejected("<Action id='A' clip='a.hkx'><HyperArmor from='0.1' until='0.5' untilFallback='end'/></Action>"),
            "windows have no event fallbacks");
        // An event moment waits for its event only (0.12.278): there is no fallback.
        for (const char* gone : {"<Action id='A' clip='a.hkx'><MoveCancel at='event:Move' fallback='0.5'/></Action>",
                 "<Action id='A' clip='a.hkx'><Exit at='event:Out' fallback='end-0.1'/></Action>",
                 "<Action id='A' clip='a.hkx'><Vanilla to='Attack' from='event:Open' fromFallback='0.3'/></Action>"}) {
            const auto p = Parse(gone);
            Check(!p.ok, "no fallback");
        }
        {
            const auto p = Parse("<Action id='A' clip='a.hkx'><MoveCancel at='0.4'/></Action>"
                                 "<Action id='B' clip='b.hkx'><MoveCancel at='event:Move'/></Action>");
            Check(p.ok && std::fabs(p.actions[0].recovery.at - 0.4f) < 1e-6f && p.actions[1].recovery.at < 0 &&
                      p.actions[1].recovery.event == "Move",
                "recovery at a clip time or at its event");
        }

        // 0.12.278: the ragdoll function; a spell no longer frees a pair's physics hold.
        {
            const auto p = Parse(std::string(kAction) + R"(<EventRegistry>
                <Event name='Down'><Do op='ragdoll'/></Event>
                <Event name='Side'><Do op='ragdoll' direction='left'/></Event>
            </EventRegistry>)");
            Check(p.ok, "ragdoll functions parse");
            const auto& down = p.events[0].functions[0];
            Check(down.op == EventOp::kRagdoll && down.direction == RagdollPush::Direction::kDown && down.force < 0,
                "a ragdoll falls down with the direction's force by default");
            const auto& side = p.events[1].functions[0];
            Check(
                side.direction == RagdollPush::Direction::kLeft && side.force < 0, "the registry holds the direction");
            const auto timed = Parse("<Pair id='P' clip='p.hkx'><Do at='0.67' op='ragdoll' "
                                     "direction='forward' target='receiver'/></Pair>");
            Check(timed.ok && timed.actions[0].pairFeatures.timed[0].function.op == EventOp::kRagdoll &&
                      timed.actions[0].pairFeatures.timed[0].function.target == EventTarget::kReceiver &&
                      timed.actions[0].pairFeatures.timed[0].function.direction == RagdollPush::Direction::kForward,
                "a pair's clip-time ragdoll names its role");
            const auto own = Parse("<Action id='A' clip='a.hkx'><Do at='0.5' op='ragdoll' "
                                   "direction='backward' force='8'/></Action>");
            Check(own.ok && own.actions[0].pairFeatures.timed[0].function.op == EventOp::kRagdoll &&
                      own.actions[0].pairFeatures.timed[0].function.force == 8.0f,
                "an ordinary action's clip-time ragdoll pushes the actor playing it");
        }
        {
            const auto p = Parse("<EventRegistry><Event name='X'><Do op='spell' plugin='a.esp' form='1' "
                                 "releasePhysics='true'/></Event></EventRegistry>");
            Check(!p.ok, "a spell has no releasePhysics");
        }
        // 0.12.293: the strike. The registry holds whom it hits and how, the values and a clip time its numbers.
        {
            const auto p = Parse(std::string(kAction) + R"(<EventRegistry>
                <Event name='Hit'><Do op='strike'/></Event>
                <Event name='Sweep'><Do op='strike' pick='area' hit='all' hand='both' block='ignore'/></Event>
            </EventRegistry>)");
            Check(p.ok, "strike functions parse");
            const auto& plain = p.events[0].functions[0].strike;
            Check(p.events[0].functions[0].op == EventOp::kStrike && plain.pick == StrikeSpec::Pick::kCombatTarget &&
                      plain.base == StrikeSpec::Base::kWeapon && plain.ratio == 1.0f && plain.duration == 0.0f &&
                      plain.hostileOnly && plain.block == StrikeSpec::Block::kVanilla,
                "a strike hits the combat target with the weapon's damage by default");
            const auto& sweep = p.events[1].functions[0].strike;
            Check(sweep.pick == StrikeSpec::Pick::kArea && !sweep.hostileOnly &&
                      sweep.hand == StrikeSpec::Hand::kBoth && sweep.block == StrikeSpec::Block::kIgnore,
                "the registry holds whom a strike hits and how");
            const auto values = Parse(std::string(kAction) +
                                      R"(<Action id='B' clip='b.hkx'><EventValues><Event name='Sweep'>
                <Do op='strike' radius='250' angle='360' up='100' down='40' edge='0.4' amount='25' duration='0.4'/>
                </Event></EventValues></Action>
                <EventRegistry><Event name='Sweep'><Do op='strike' pick='area'/></Event></EventRegistry>)");
            Check(values.ok, "a strike's numbers parse in EventValues");
            if (values.ok) {
                const auto run =
                    WithValues(values.events[0].functions[0], values.actions[1].eventValues[0].functions[0]);
                Check(run.op == EventOp::kStrike && run.strike.pick == StrikeSpec::Pick::kArea &&
                          run.strike.radius == 250.0f && run.strike.angle == 360.0f && run.strike.up == 100.0f &&
                          run.strike.down == 40.0f && std::fabs(run.strike.edge - 0.4f) < 1e-6f &&
                          run.strike.base == StrikeSpec::Base::kFixed && run.strike.amount == 25.0f &&
                          std::fabs(run.strike.duration - 0.4f) < 1e-6f,
                    "a registered strike runs with the registry's pick and the action's numbers");
            }
            const auto timed = Parse("<Pair id='P' clip='p.hkx'><Do at='0.42' op='strike' pick='partner' "
                                     "ratio='1.5' block='ignore' target='initiator'/></Pair>");
            Check(timed.ok && timed.actions[0].pairFeatures.timed[0].function.op == EventOp::kStrike &&
                      timed.actions[0].pairFeatures.timed[0].function.strike.pick == StrikeSpec::Pick::kPartner &&
                      timed.actions[0].pairFeatures.timed[0].function.strike.ratio == 1.5f,
                "a pair's clip-time strike names its attacker and target");
            const auto own = Parse("<Action id='A' clip='a.hkx'><Do at='0.3' op='strike' pick='area' angle='360' "
                                   "radius='250' duration='0.4'/></Action>");
            Check(own.ok && own.actions[0].pairFeatures.timed[0].function.strike.angle == 360.0f,
                "an ordinary action's clip-time strike is its own actor's");
        }
        // 0.12.294: after the damage, a reaction and what a real hit brings along.
        {
            const auto p = Parse(std::string(kAction) + R"(<EventRegistry>
                <Event name='Bash'><Do op='strike' reaction='stagger' without='poison,hitEvent'/></Event>
            </EventRegistry>
            <Action id='B' clip='b.hkx'><EventValues><Event name='Bash'><Do op='strike' strength='0.8'/></Event>
            </EventValues></Action>)");
            Check(p.ok, "a strike's reaction parses");
            if (p.ok) {
                const auto& reg = p.events[0].functions[0].strike;
                Check(reg.reaction == StrikeSpec::Reaction::kStagger && reg.enchantment && !reg.poison && reg.perks &&
                          reg.impact && !reg.hitEvent,
                    "the registry holds the reaction and what is left out");
                const auto run = WithValues(p.events[0].functions[0], p.actions[1].eventValues[0].functions[0]);
                Check(run.strike.reaction == StrikeSpec::Reaction::kStagger && !run.strike.poison &&
                          std::fabs(run.strike.stagger - 0.8f) < 1e-6f,
                    "a registered strike runs with the registry's reaction and the action's strength");
            }
            const auto plain = Parse("<Action id='A' clip='a.hkx'><Do at='0.3' op='strike'/></Action>");
            Check(plain.ok &&
                      plain.actions[0].pairFeatures.timed[0].function.strike.reaction == StrikeSpec::Reaction::kNone &&
                      plain.actions[0].pairFeatures.timed[0].function.strike.hitEvent,
                "a strike has no reaction and brings everything along by default");
            const auto push = Parse("<Action id='A' clip='a.hkx'><Do at='0.3' op='strike' reaction='push' "
                                    "force='8'/></Action>");
            Check(push.ok && push.actions[0].pairFeatures.timed[0].function.strike.force == 8.0f,
                "a push takes its force");
        }
        // 0.12.296: a sound, a global and a mod event.
        {
            const auto p = Parse(std::string(kAction) + R"(<EventRegistry>
                <Event name='Snd'><Do op='sound' file='sound\fx\mymod\whoosh.wav' bone='rightHand'/></Event>
                <Event name='Reg'><Do op='sound' plugin='Skyrim.esm' form='3C7BA'/></Event>
                <Event name='Glob'><Do op='globalSet' plugin='MyMod.esp' form='801' restore='end'/></Event>
                <Event name='Mod'><Do op='modEvent' name='MyMod_Hit' text='heavy'/></Event>
            </EventRegistry>
            <Action id='B' clip='b.hkx'><EventValues>
                <Event name='Snd'><Do op='sound' volume='0.5'/></Event>
                <Event name='Glob'><Do op='globalSet' value='2'/></Event>
                <Event name='Mod'><Do op='modEvent' number='3'/></Event>
            </EventValues></Action>)");
            Check(p.ok, "sound, global and mod event functions parse");
            if (p.ok) {
                const auto& snd = p.events[0].functions[0];
                Check(
                    snd.op == EventOp::kSound && snd.file == "sound\\fx\\mymod\\whoosh.wav" && snd.bone == "rightHand",
                    "a sound holds its file and bone");
                Check(p.events[1].functions[0].plugin == "Skyrim.esm" && p.events[1].functions[0].bone == "spine",
                    "a registered sound plays at the spine by default");
                const auto& glob = p.events[2].functions[0];
                Check(glob.op == EventOp::kGlobalSet && glob.restore, "a global set keeps its restore");
                const auto run = WithValues(glob, p.actions[1].eventValues[1].functions[0]);
                Check(run.restore && run.value == 2.0f && run.plugin == "MyMod.esp",
                    "a global runs with the action's value");
                const auto mod = WithValues(p.events[3].functions[0], p.actions[1].eventValues[2].functions[0]);
                Check(mod.name == "MyMod_Hit" && mod.text == "heavy" && mod.number == 3.0f,
                    "a mod event runs with the registry's name and the action's number");
                const auto sound = WithValues(snd, p.actions[1].eventValues[0].functions[0]);
                Check(sound.volume == 0.5f && sound.file == snd.file, "a sound runs with the action's volume");
            }
            const auto own = Parse("<Action id='A' clip='a.hkx'><Do at='0.2' op='sound' file='sound\\fx\\a.wav'/>"
                                   "<Do at='0.4' op='modEvent' name='X'/><Do at='0.5' op='globalSet' plugin='a.esp' "
                                   "form='1' value='1'/></Action>");
            Check(own.ok && own.actions[0].pairFeatures.timed.size() == 3, "an ordinary action's clip time takes them");
        }
        Check(Rejected("<EventRegistry><Event name='X'><Do op='sound'/></Event></EventRegistry>"),
            "a sound needs a file or a registered sound");
        Check(Rejected("<EventRegistry><Event name='X'><Do op='sound' file='a.wav' plugin='a.esp' form='1'/></Event>"
                       "</EventRegistry>"),
            "a sound takes a file or a registered sound, not both");
        Check(Rejected("<EventRegistry><Event name='X'><Do op='sound' file='C:\\a.wav'/></Event></EventRegistry>"),
            "a sound's file is under Data");
        Check(Rejected(
                  "<EventRegistry><Event name='X'><Do op='sound' file='a.wav' volume='0.5'/></Event></EventRegistry>"),
            "a sound's volume is the action's");
        Check(Rejected("<EventRegistry><Event name='X'><Do op='modEvent'/></Event></EventRegistry>"),
            "a mod event needs a name");
        Check(Rejected("<Action id='A' clip='a.hkx'><EventValues><Event name='X'><Do op='modEvent' name='Y'/></Event>"
                       "</EventValues></Action>"),
            "a mod event's name is the registry's");
        Check(Rejected("<Action id='A' clip='a.hkx'><Do at='0.2' op='hide' value='1'/></Action>"),
            "value belongs to globalSet");
        Check(Rejected("<Action id='A' clip='a.hkx'><Do at='0.3' op='strike' strength='0.5'/></Action>"),
            "a strike's strength is a stagger's");
        Check(
            Rejected("<Action id='A' clip='a.hkx'><Do at='0.3' op='strike' reaction='stagger' strength='2'/></Action>"),
            "a stagger's strength is at most 1");
        Check(Rejected("<Action id='A' clip='a.hkx'><Do at='0.3' op='strike' reaction='stagger' force='3'/></Action>"),
            "a strike's force is a push's");
        Check(Rejected("<Action id='A' clip='a.hkx'><Do at='0.3' op='strike' without='blood'/></Action>"),
            "without names what a hit brings along");
        Check(Rejected("<Action id='A' clip='a.hkx'><EventValues><Event name='X'><Do op='strike' "
                       "reaction='push'/></Event></EventValues></Action>"),
            "the reaction is the registry's");
        Check(Rejected("<EventRegistry><Event name='X'><Do op='strike' radius='200'/></Event></EventRegistry>"),
            "a strike's numbers are the action's, not the registry's");
        Check(Rejected("<Action id='A' clip='a.hkx'><EventValues><Event name='X'><Do op='strike' pick='area'/></Event>"
                       "</EventValues></Action>"),
            "whom a strike hits is the registry's");
        Check(Rejected("<Action id='A' clip='a.hkx'><Do at='0.3' op='strike' ratio='1' amount='5'/></Action>"),
            "a strike takes a ratio or an amount, not both");
        Check(Rejected("<Action id='A' clip='a.hkx'><Do at='0.3' op='strike' pick='partner' radius='200'/></Action>"),
            "area numbers belong to pick='area'");
        Check(Rejected("<Action id='A' clip='a.hkx'><Do at='0.3' op='ragdoll' pick='area'/></Action>"),
            "strike settings belong to strike");
        Check(Rejected("<EventRegistry><Event name='X'><Do op='ragdoll' direction='north'/></Event></EventRegistry>"),
            "a ragdoll's direction is one of six");
        Check(Rejected("<Action id='A' clip='a.hkx'><EventValues><Event name='X'><Do op='ragdoll' force='21'/></Event>"
                       "</EventValues></Action>"),
            "a ragdoll's force is at most 20");

        // 0.12.281: the registry holds the events' functions and what they act with; the numbers are the actions'.
        {
            for (const char* number : {"delay='0.4'", "strength='1.5'", "duration='1'", "stamina='5'", "factor='0.5'",
                     "mode='direct'", "force='3'"}) {
                const auto r = Parse(std::string("<EventRegistry><Event name='X'><Do op='shake' ") + number +
                                     "/></Event></EventRegistry>");
                Check(!r.ok && r.error.find("0.12.281") != std::string::npos &&
                          r.error.find("EventValues") != std::string::npos,
                    "a number in the registry is refused, naming EventValues");
            }
            const auto p = Parse(R"(<Action id='A' clip='a.hkx'><EventValues>
                <Event name='Fire'><Do op='fireArrow' mode='direct' damage='150'/><Do op='shake' strength='2' delay='0.2'/></Event>
                <Event name='Throw'><Do op='ragdoll' force='8'/></Event>
                <Event name='Pay'><Do op='cost' stamina='12'/></Event>
            </EventValues></Action>
            <Pair id='P' clip='p.hkx'><EventValues><Event name='Fire'><Do op='spell' delay='0.3'/></Event></EventValues></Pair>
            <EventRegistry>
                <Event name='Fire'><Do op='fireArrow'/><Do op='shake'/></Event>
                <Event name='Throw'><Do op='ragdoll' direction='backward'/></Event>
                <Event name='Pay'><Do op='cost'/></Event>
            </EventRegistry>)");
            Check(p.ok, p.error.c_str());
            const auto& v = p.actions[0].eventValues;
            Check(v.size() == 3 && v[0].event == "Fire" && v[0].functions.size() == 2, "an action's values by event");
            Check(v[0].functions[0].op == EventOp::kFireArrow &&
                      v[0].functions[0].arrow.mode == FireArrowMode::kDirect &&
                      std::fabs(v[0].functions[0].arrow.damage - 1.5f) < 1e-6f,
                "a direct shot's numbers");
            Check(v[0].functions[1].op == EventOp::kShake && std::fabs(v[0].functions[1].strength - 2) < 1e-6f &&
                      std::fabs(v[0].functions[1].delay - 0.2f) < 1e-6f,
                "a shake's numbers and delay");
            Check(std::fabs(v[1].functions[0].force - 8) < 1e-6f && std::fabs(v[2].functions[0].stamina - 12) < 1e-6f,
                "a ragdoll's force, a cost");
            Check(p.actions[1].eventValues.size() == 1 &&
                      std::fabs(p.actions[1].eventValues[0].functions[0].delay - 0.3f) < 1e-6f,
                "a pair's values");
            const auto& reg = p.events;
            Check(reg[1].functions[0].direction == RagdollPush::Direction::kBackward,
                "the ragdoll's direction stays registered");

            // A registry function run with an action's numbers keeps what it acts with.
            EventFunctionSpec registry = reg[1].functions[0];
            registry.plugin = "a.esp";
            const auto run = WithValues(registry, v[1].functions[0]);
            Check(run.op == EventOp::kRagdoll && run.direction == RagdollPush::Direction::kBackward &&
                      run.plugin == "a.esp" && std::fabs(run.force - 8) < 1e-6f && run.target == EventTarget::kSender,
                "WithValues: the registry's direction, the action's force");
            Check(NeedsValues(EventOp::kCost) && NeedsValues(EventOp::kTimeOn) && !NeedsValues(EventOp::kShake) &&
                      !NeedsValues(EventOp::kSpell),
                "cost and timeOn run only with an action's numbers");
        }
        for (const char* attr : {"plugin='a.esp' form='1'", "direction='up'", "target='player'", "at='0.1'"}) {
            const auto r =
                Parse(std::string("<Action id='A' clip='a.hkx'><EventValues><Event name='X'><Do op='ragdoll' ") + attr +
                      "/></Event></EventValues></Action>");
            Check(!r.ok && r.error.find("0.12.281") != std::string::npos, "an action's values are numbers only");
        }
        Check(
            Rejected(
                "<Action id='A' clip='a.hkx'><EventValues><Event name='X'><Do op='cost'/></Event></EventValues></Action>"),
            "a cost's values give an amount");
        Check(
            Rejected(
                "<Action id='A' clip='a.hkx'><EventValues><Event name='X'><Do op='timeOn'/></Event></EventValues></Action>"),
            "a time scale's values give a factor");
        Check(Rejected("<Action id='A' clip='a.hkx'><EventValues><Event name='X'><Do op='hide'/></Event>"
                       "<Event name='x'><Do op='hide'/></Event></EventValues></Action>"),
            "one event's values once");
        Check(Rejected("<Action id='A' clip='a.hkx'><EventValues><Event name='X'/></EventValues></Action>"),
            "an event's values hold at least one function");
        Check(Rejected("<Start id='S' key='E' to='A'><EventValues><Event name='X'><Do op='hide'/></Event>"
                       "</EventValues></Start>" +
                       std::string(kAction)),
            "a start plays no clip: no values");
        // MatchValues: by op, in order among that op; extras are reported.
        {
            auto fn = [](EventOp op, float delay = 0) {
                EventFunctionSpec f;
                f.op = op;
                f.delay = delay;
                return f;
            };
            const std::vector<EventFunctionSpec> registry = {
                fn(EventOp::kSpell), fn(EventOp::kShake), fn(EventOp::kSpell), fn(EventOp::kHide)};
            const std::vector<EventFunctionSpec> values = {
                fn(EventOp::kSpell, 1), fn(EventOp::kSpell, 2), fn(EventOp::kSpell, 3), fn(EventOp::kShake, 4)};
            std::vector<int> unmatched;
            const auto m = MatchValues(registry, values, &unmatched);
            Check(m == std::vector<int>{0, 3, 1, -1}, "each op's values in order, none for the hide");
            Check(unmatched == std::vector<int>{2}, "the third spell's values have no function");
            Check(MatchValues(registry, {}) == std::vector<int>(4, -1), "no values: defaults");
        }
        Check(Rejected("<EventRegistry><Event name='X'><Do op='hide' direction='down'/></Event></EventRegistry>"),
            "direction and force only for a ragdoll");
        {
            using namespace RagdollPush;
            auto same = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };
            // Facing +Y (yaw 0): a forward push comes from behind.
            const auto behind = PushPoint(Direction::kForward, 10, 20, 30, 0);
            Check(same(behind.x, 10) && same(behind.y, -30) && same(behind.z, 80), "forward: the point behind");
            // Facing +X (yaw 90 degrees): the right is -Y, where a left push comes from.
            const auto right = PushPoint(Direction::kLeft, 0, 0, 0, 1.5707963f);
            Check(same(right.x, 0) && same(right.y, -50) && same(right.z, 50), "left: the point on the right");
            Check(same(PushPoint(Direction::kDown, 1, 2, 3, 2.0f).z, 163), "down: the point above");
            Check(ForceOf(Direction::kForward, -1) == 5 && ForceOf(Direction::kDown, -1) == 1 &&
                      ForceOf(Direction::kUp, 30) == kMaxForce && ForceOf(Direction::kUp, 3) == 3,
                "force: the direction's default, the written value, at most 20");
        }
        Check(Rejected("<Action id='A' clip='a.hkx'><Cost at='event:Pay' stamina='5'/></Action>"),
            "costs are paid at clip times");

        // A pair's clip-time effects.
        {
            const auto p = Parse(R"(<Pair id='P' clip='p.hkx'>
                <Camera reset='Reset' restore='event' timeout='5'/>
                <Do at='0.15' op='hide' target='both'/>
                <Do at='0.2' op='cost' target='player' stamina='10'/>
            </Pair>)");
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
            Check(Rejected("<Pair id='P' clip='p.hkx'><Do op='hide'/></Pair>"), "a pair's Do needs its clip time");
            Check(Rejected("<Pair id='P' clip='p.hkx'><Do at='1' op='fireArrow'/></Pair>"),
                "a clip-time shot is the action's FireArrow");
            // 0.12.275: a clip-time effect names the role it changes; without one, both. No sender filter.
            const auto plain = Parse("<Pair id='P' clip='p.hkx'><Do at='0.1' op='hide'/></Pair>");
            Check(plain.ok && plain.actions[0].pairFeatures.timed[0].function.target == EventTarget::kBoth,
                "a clip-time effect without a target changes both");
            for (const char* attr : {"from='any'", "from='initiator'", "target='sender'", "target='partner'"}) {
                const auto r =
                    Parse(std::string("<Pair id='P' clip='p.hkx'><Do at='0.1' op='hide' ") + attr + "/></Pair>");
                Check(!r.ok, "no sender on a clip time");
            }
            for (const char* attr : {"space='back'", "minSpace='1'", "maxSpace='100'"}) {
                const auto r = Parse(
                    std::string("<EventRegistry><Event name='X'><Do op='hide' ") + attr + "/></Event></EventRegistry>");
                Check(!r.ok, "a function has no space condition");
            }
        }
        // An ordinary action's camera restore (0.12.260): Camera alone; the other effects stay a pair's.
        {
            const auto p =
                Parse("<Action id='A' clip='a.hkx'><Camera reset='Back' restore='event' timeout='3'/></Action>");
            Check(p.ok, "an ordinary action takes a camera restore");
            const auto& f = p.actions[0].pairFeatures;
            Check(f.cameraReset == "Back" && !f.cameraRestoreOnEnd && std::fabs(f.cameraTimeout - 3) < 1e-6f,
                "its camera settings");
            Check(Parse("<Action id='A' clip='a.hkx'><Camera/></Action>").ok, "restored at its end by default");
            // 0.12.274: clip-time effects on the actor playing it, the ones its own slots do not cover.
            const auto timed = Parse("<Action id='A' clip='a.hkx'><Do at='0.1' op='hide'/>"
                                     "<Do at='0.5' op='shake' strength='2' duration='1'/></Action>");
            Check(timed.ok && timed.actions[0].pairFeatures.timed.size() == 2 &&
                      timed.actions[0].pairFeatures.timed[1].function.op == EventOp::kShake &&
                      std::fabs(timed.actions[0].pairFeatures.timed[1].at - 0.5f) < 1e-6f,
                "an ordinary action's clip-time effects");
            for (const char* op : {"iframeOn", "armorOn", "rotateOn", "cost", "cancel", "fireArrow"})
                Check(Rejected(std::string("<Action id='A' clip='a.hkx'><Do at='0.1' op='") + op + "'/></Action>"),
                    (std::string("not an ordinary action's clip-time effect: ") + op).c_str());
            Check(Rejected("<Action id='A' clip='a.hkx'><Do at='0.1' op='hide' target='player'/>"
                           "</Action>"),
                "no target: the actor playing it");
            Check(Rejected("<Action id='A' clip='a.hkx' interruptOnHit='true'/>"),
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

        // 0.12.274: an annotation's numbers are AI.CamShake's (runtime/AnnotationCommands.h); registered
        // "name|value" spellings and shake parameters are gone, and "AI" is no declared event name.
        {
            const auto refused = [](const std::string& body) {
                const auto p = Parse(body);
                return !p.ok;
            };
            Check(refused("<EventRegistry><Event name='X' values='1.5|0.5'/></EventRegistry>"), "values refused");
            Check(refused("<EventRegistry><Event name='X'><Do op='shake' parameters='true'/></Event></EventRegistry>"),
                "parameters refused");
            Check(refused("<EventRegistry><Event name='ai'/></EventRegistry>"), "AI is reserved, ignoring case");
            Check(refused("<Action id='A' clip='a.hkx'><Exit at='event:AI'/></Action>"), "in any event attribute");
            using AnnotationCommands::Parse;
            using AnnotationCommands::Parsed;
            Check(Parse("CamShake|1.5|0.5") == Parsed{AnnotationCommands::Command::kCamShake, 1.5f, 0.5f},
                "strength and duration");
            Check(Parse("camshake|2|1") == Parsed{AnnotationCommands::Command::kCamShake, 2.0f, 1.0f}, "any case");
            Check(Parse("CamShake|0|0") && Parse("CamShake|10|30"), "the range ends");
            for (const char* bad : {"", "CamShake", "CamShake|", "CamShake|1|", "CamShake||1", "CamShake|1|1|1",
                     "CamShake|x", "CamShake|1x", "CamShake|-1", "CamShake|11", "CamShake|1|31", "CamShake|nan",
                     "CamShake|inf", "@CamShake|1", "Shake|1|1", " CamShake|1", "CamShake|1.5"})
                Check(!Parse(bad), (std::string("refused: '") + bad + "'").c_str());
            // A pair's two roles sending the same command at one moment run it once (0.12.275).
            AnnotationCommands::PairEcho echo;
            const Parsed shake{AnnotationCommands::Command::kCamShake, 1.5f, 0.5f};
            Check(!echo.Repeat(1, 2, shake, 10.0) && echo.Repeat(2, 1, shake, 10.05), "the partner's echo is skipped");
            Check(!echo.Repeat(2, 1, shake, 10.5), "a later one runs");
            Check(!echo.Repeat(1, 0, shake, 20.0) && !echo.Repeat(1, 0, shake, 20.01), "outside a pair every one runs");
            Check(!echo.Repeat(3, 4, shake, 30.0) &&
                      !echo.Repeat(4, 3, Parsed{AnnotationCommands::Command::kCamShake, 1.0f, 0.5f}, 30.01),
                "other numbers run");
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
            EventRuleSpec a{"Ragdoll", {}, "A.xml", p.events[0].qtes};
            EventRuleSpec b{"ragdoll", {}, "B.xml", p.events[0].qtes};
            Check(EventRegistryMerge::Merge({a, b}).entries[0].qtes.size() == 2, "every file's QTEs");
        }
        std::cout << checks << " event registry checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
