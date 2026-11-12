#include "runtime/AnnotationCommands.h"
#include "runtime/ClipWindowsJudge.h"
#include "Settings.h"
#include "declarations/XmlV2.h"
#include "runtime/EventRegistryMerge.h"
#include "runtime/EventValuesMatch.h"
#include "runtime/ResourceCost.h"
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
                <Event name="Grapple_Stom"><Do op="magic" action="cast" plugin="FH_Grapple.esp" form="0x80B"/></Event>
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
            Check(spell.op == EventOp::kMagicCast && spell.plugin == "FH_Grapple.esp" && spell.form == "80B" &&
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
        Check(Rejected("<EventRegistry><Event name='X'><Do op='magic' action='cast'/></Event></EventRegistry>"),
            "a spell needs its plugin and form");
        // 11-01: one magic function, its action says cast, add or remove; the old ops are gone.
        {
            const auto p = Parse(R"(<EventRegistry><Event name='M'>
                <Do op='magic' action='add' plugin='a.esp' form='1'/>
                <Do op='magic' action='remove' plugin='a.esp' form='1'/>
                <Do op='magic' action='cast' plugin='a.esp' form='1'/>
            </Event></EventRegistry>)");
            Check(p.ok && p.events[0].functions.size() == 3 && p.events[0].functions[0].op == EventOp::kMagicAdd &&
                      p.events[0].functions[1].op == EventOp::kMagicRemove &&
                      p.events[0].functions[2].op == EventOp::kMagicCast,
                "magic add, remove and cast");
        }
        for (const char* bad :
            {"<Do op='magic' plugin='a.esp' form='1'/>", "<Do op='magic' action='learn' plugin='a.esp' form='1'/>",
                "<Do op='hide' action='add'/>", "<Do op='spell' plugin='a.esp' form='1'/>",
                "<Do op='stateOn' plugin='a.esp' form='1'/>", "<Do op='stateOff' plugin='a.esp' form='1'/>"})
            Check(Rejected(std::string("<EventRegistry><Event name='X'>") + bad + "</Event></EventRegistry>"),
                (std::string("rejected: ") + bad).c_str());
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
        Check(Rejected("<Action id='A' clip='a.hkx'><FireArrow/></Action>"),
            "FireArrow is gone (0.12.302): the action's shot is a fireArrow function");
        Check(Rejected("<Action id='A' clip='a.hkx'><IFrame from='event:On' until='end'/></Action>"),
            "windows open at clip times");
        Check(Rejected("<Action id='A' clip='a.hkx'><HyperArmor from='0.1' until='0.5' untilFallback='end'/></Action>"),
            "windows have no event fallbacks");
        // An event moment waits for its event only (0.12.278): there is no fallback.
        for (const char* gone : {"<Action id='A' clip='a.hkx'><MoveCancel at='event:Move' fallback='0.5'/></Action>",
                 "<Action id='A' clip='a.hkx'><Exit at='event:Out' fallback='end-0.1'/></Action>"}) {
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
            const auto p = Parse("<EventRegistry><Event name='X'><Do op='magic' action='cast' plugin='a.esp' form='1' "
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
                     "damage='80'", "force='3'"}) {
                const auto r = Parse(std::string("<EventRegistry><Event name='X'><Do op='shake' ") + number +
                                     "/></Event></EventRegistry>");
                Check(!r.ok && r.error.find("0.12.281") != std::string::npos &&
                          r.error.find("EventValues") != std::string::npos,
                    "a number in the registry is refused, naming EventValues");
            }
            const auto p = Parse(R"(<Action id='A' clip='a.hkx'><EventValues>
                <Event name='Fire'><Do op='fireArrow' damage='150'/><Do op='shake' strength='2' delay='0.2'/></Event>
                <Event name='Throw'><Do op='ragdoll' force='8'/></Event>
                <Event name='Pay'><Do op='cost' stamina='12'/></Event>
            </EventValues></Action>
            <Pair id='P' clip='p.hkx'><EventValues><Event name='Fire'><Do op='magic' action='cast' delay='0.3'/></Event></EventValues></Pair>
            <EventRegistry>
                <Event name='Fire'><Do op='fireArrow'/><Do op='shake'/></Event>
                <Event name='Throw'><Do op='ragdoll' direction='backward'/></Event>
                <Event name='Pay'><Do op='cost'/></Event>
            </EventRegistry>)");
            Check(p.ok, p.error.c_str());
            const auto& v = p.actions[0].eventValues;
            Check(v.size() == 3 && v[0].event == "Fire" && v[0].functions.size() == 2, "an action's values by event");
            Check(
                v[0].functions[0].op == EventOp::kFireArrow && std::fabs(v[0].functions[0].arrowDamage - 1.5f) < 1e-6f,
                "a shot's damage");
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
                      !NeedsValues(EventOp::kMagicCast),
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
                fn(EventOp::kMagicCast), fn(EventOp::kShake), fn(EventOp::kMagicCast), fn(EventOp::kHide)};
            const std::vector<EventFunctionSpec> values = {fn(EventOp::kMagicCast, 1), fn(EventOp::kMagicCast, 2),
                fn(EventOp::kMagicCast, 3), fn(EventOp::kShake, 4)};
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
        // Health as a cost too (10-31), never its last point.
        {
            const auto c = Parse("<Action id='A' clip='a.hkx'><Cost health='15' magicka='5'/></Action>");
            Check(c.ok && c.actions[0].cost.enabled && std::fabs(c.actions[0].cost.health - 15) < 1e-6f &&
                      std::fabs(c.actions[0].cost.magicka - 5) < 1e-6f && c.actions[0].cost.stamina == 0,
                "a health cost");
            const auto t = Parse("<Pair id='P' clip='p.hkx'><Do at='0.2' op='cost' health='8'/></Pair>");
            Check(t.ok && std::fabs(t.actions[0].pairFeatures.timed[0].function.health - 8) < 1e-6f,
                "a clip-time health cost");
            Check(Rejected("<Action id='A' clip='a.hkx'><Cost health='-1'/></Action>"), "no negative health cost");
            using AnimationInjector::ResourceCost::Health;
            Check(Health(15, 100) == 15 && Health(15, 10) == 9 && Health(15, 1) == 0 && Health(15, 0.5f) == 0 &&
                      Health(-3, 100) == 0,
                "health cost leaves the last point");
        }
        // A cost in percent of each maximum (10-31): the action's Cost, and a registered cost with its node's numbers.
        {
            const auto c = Parse("<Action id='A' clip='a.hkx'><Cost stamina='25' measure='percent'/></Action>");
            Check(c.ok && c.actions[0].cost.percent && std::fabs(c.actions[0].cost.stamina - 25) < 1e-6f,
                "a percent cost");
            Check(!Parse("<Action id='A' clip='a.hkx'><Cost stamina='25'/></Action>").actions[0].cost.percent,
                "values by default");
            Check(Rejected("<Action id='A' clip='a.hkx'><Cost stamina='120' measure='percent'/></Action>"),
                "at most 100 percent");
            Check(
                Rejected("<Action id='A' clip='a.hkx'><Cost stamina='5' measure='max'/></Action>"), "value or percent");
            const auto r = Parse(R"(<Action id='A' clip='a.hkx'><EventValues>
                <Event name='Pay'><Do op='cost' health='10'/></Event></EventValues></Action>
                <EventRegistry><Event name='Pay'><Do op='cost' measure='percent'/></Event></EventRegistry>)");
            Check(r.ok, r.error.c_str());
            const auto run = WithValues(r.events[0].functions[0], r.actions[0].eventValues[0].functions[0]);
            Check(run.costPercent && std::fabs(run.health - 10) < 1e-6f, "the registry's unit, the node's numbers");
            Check(Rejected("<Action id='A' clip='a.hkx'><EventValues><Event name='Pay'><Do op='cost' stamina='5' "
                           "measure='percent'/></Event></EventValues></Action>"),
                "the unit is the registry's");
            Check(Rejected("<EventRegistry><Event name='X'><Do op='shake' measure='percent'/></Event></EventRegistry>"),
                "measure is a cost's");
            using AnimationInjector::ResourceCost::OfMaximum;
            Check(OfMaximum(25, 200) == 50 && OfMaximum(150, 200) == 200 && OfMaximum(-5, 200) == 0 &&
                      OfMaximum(25, 0) == 0,
                "percent of the maximum, within 0~100");
        }

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
                "a shot is an ordinary action's");
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
            for (const char* op : {"iframeOn", "armorOn", "rotateOn", "cost", "cancel"})
                Check(Rejected(std::string("<Action id='A' clip='a.hkx'><Do at='0.1' op='") + op + "'/></Action>"),
                    (std::string("not an ordinary action's clip-time effect: ") + op).c_str());
            Check(Rejected("<Action id='A' clip='a.hkx'><Do at='0.1' op='hide' target='player'/>"
                           "</Action>"),
                "no target: the actor playing it");
            Check(Rejected("<Action id='A' clip='a.hkx' interruptOnHit='true'/>"),
                "third-party interruption stays a pair's");
        }

        // A shot (0.12.303): the direct one only, at its event or clip time; a clip-time one is a timed effect.
        {
            const auto p = Parse("<Action id='A' clip='a.hkx'><Do at='0.5' op='fireArrow' damage='50'/>"
                                 "<Do at='0.9' op='fireArrow'/></Action>");
            const auto& timed = p.actions[0].pairFeatures.timed;
            Check(p.ok && timed.size() == 2 && timed[0].function.op == EventOp::kFireArrow &&
                      std::fabs(timed[0].at - 0.5f) < 1e-6f &&
                      std::fabs(timed[0].function.arrowDamage - 0.5f) < 1e-6f &&
                      std::fabs(timed[1].function.arrowDamage - 1.0f) < 1e-6f,
                "clip-time shots are timed effects, more than one");
            for (const char* attr : {"mode='direct'", "mode='drawn'", "onRelease='true'", "delay='0.2'"}) {
                Check(Rejected(std::string("<Action id='A' clip='a.hkx'><Do at='0.5' op='fireArrow' ") + attr +
                               "/></Action>"),
                    (std::string("a shot has no ") + attr).c_str());
                Check(Rejected(std::string("<EventRegistry><Event name='Fire'><Do op='fireArrow'/></Event>"
                                           "</EventRegistry><Action id='A' clip='a.hkx'><EventValues><Event "
                                           "name='Fire'><Do op='fireArrow' ") +
                               attr + "/></Event></EventValues></Action>"),
                    (std::string("an action's shot values have no ") + attr).c_str());
            }
            Check(Rejected("<Action id='A' clip='a.hkx'><Do at='0.5' op='hide' damage='50'/></Action>"),
                "damage is a shot's");
        }

        // Every file's registry merged: all functions run, equal ones once, names ignore case.
        {
            EventFunctionSpec hide;
            hide.op = EventOp::kHide;
            EventFunctionSpec spell;
            spell.op = EventOp::kMagicCast;
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
            const auto shakeOf = [](float strength, float duration) {
                Parsed p;
                p.op = EventOp::kShake;
                p.strength = strength;
                p.duration = duration;
                return p;
            };
            Check(Parse("CamShake|1.5|0.5") == shakeOf(1.5f, 0.5f), "strength and duration");
            Check(Parse("camshake|2|1") == shakeOf(2.0f, 1.0f), "any case");
            Check(Parse("CamShake|0|0") && Parse("CamShake|10|30"), "the range ends");
            for (const char* bad : {"", "CamShake", "CamShake|", "CamShake|1|", "CamShake||1", "CamShake|1|1|1",
                     "CamShake|x", "CamShake|1x", "CamShake|-1", "CamShake|11", "CamShake|1|31", "CamShake|nan",
                     "CamShake|inf", "@CamShake|1", "Shake|1|1", " CamShake|1", "CamShake|1.5"})
                Check(!Parse(bad), (std::string("refused: '") + bad + "'").c_str());
            // A pair's two roles sending the same command at one moment run it once (0.12.275).
            AnnotationCommands::PairEcho echo;
            const auto shake = shakeOf(1.5f, 0.5f);
            Check(!echo.Repeat(1, 2, shake, 10.0) && echo.Repeat(2, 1, shake, 10.05), "the partner's echo is skipped");
            Check(!echo.Repeat(2, 1, shake, 10.5), "a later one runs");
            Check(!echo.Repeat(1, 0, shake, 20.0) && !echo.Repeat(1, 0, shake, 20.01), "outside a pair every one runs");
            Check(
                !echo.Repeat(3, 4, shake, 30.0) && !echo.Repeat(4, 3, shakeOf(1.0f, 0.5f), 30.01), "other numbers run");
        }
        // Every event function but a strike is an AI. command (11-02): the annotation's own values, a window's on
        // with an optional span last.
        {
            using AnnotationCommands::Parse;
            const auto op = [](const char* text, EventOp want) {
                const auto p = Parse(text);
                return p && p->op == want;
            };
            Check(op("IFrameOn", EventOp::kIFrameOn) && op("iframeoff", EventOp::kIFrameOff) &&
                      op("ArmorOn", EventOp::kArmorOn) && op("ArmorOff", EventOp::kArmorOff) &&
                      op("RotateOn", EventOp::kRotateOn) && op("RotateOff", EventOp::kRotateOff) &&
                      op("TimeOff", EventOp::kTimeOff) && op("Hide", EventOp::kHide) && op("Show", EventOp::kShow) &&
                      op("StopVelocity", EventOp::kStopVelocity) && op("CancelPair", EventOp::kCancel),
                "the commands without values");
            Check(Parse("IFrameOn")->span == 0.0f && Parse("IFrameOn|0.3")->span == 0.3f &&
                      Parse("ArmorOn|60")->span == 60.0f && Parse("RotateOn|0")->span == 0.0f,
                "a window's on takes an optional span");
            const auto time = Parse("TimeOn|0.3|0.15|1.5");
            Check(time && time->op == EventOp::kTimeOn && time->factor == 0.3f && time->ease == 0.15f &&
                      time->span == 1.5f && Parse("TimeOn|0.3|0.15")->span == 0.0f,
                "a time scale: factor, ease, then its span");
            const auto magic = Parse("MagicAdd|Skyrim.esm|0x12FCD");
            Check(magic && magic->op == EventOp::kMagicAdd && magic->plugin == "Skyrim.esm" && magic->form == "12FCD" &&
                      op("MagicRemove|a.esp|800", EventOp::kMagicRemove) &&
                      op("MagicCast|a.esp|800", EventOp::kMagicCast),
                "magic: a plugin and a form");
            const auto cost = Parse("Cost|20|0|5"), percent = Parse("CostPercent|10|0|0");
            Check(cost && cost->op == EventOp::kCost && cost->stamina == 20.0f && cost->health == 5.0f &&
                      !cost->costPercent && percent && percent->costPercent && percent->stamina == 10.0f,
                "a cost as amounts or percent");
            const auto ragdoll = Parse("Ragdoll|backward|8");
            Check(ragdoll && ragdoll->direction == RagdollPush::Direction::kBackward && ragdoll->force == 8.0f,
                "a ragdoll's direction and force");
            Check(Parse("FireArrow|150")->arrowDamage == 1.5f, "a shot's damage in percent");
            const auto sound = Parse("Sound|sound\\fx\\a.wav|head|0.8"), form = Parse("SoundForm|a.esp|3C|root|1");
            Check(sound && sound->file == "sound\\fx\\a.wav" && sound->bone == "head" && sound->volume == 0.8f &&
                      form && form->plugin == "a.esp" && form->form == "3C" && form->bone == "root",
                "a sound by file or by form");
            const auto global = Parse("GlobalSet|a.esp|D62|-2.5"), mod = Parse("ModEvent|MyDodge||1");
            Check(global && global->value == -2.5f && !global->restore && mod && mod->name == "MyDodge" &&
                      mod->text.empty() && mod->number == 1.0f,
                "a global's value, a mod event's name, text and number");
            for (const char* bad : {"IFrameOn|61", "IFrameOn|-1", "IFrameOn|1|1", "IFrameOff|1", "TimeOn|0.3",
                     "TimeOn|0.01|0", "TimeOn|1|3", "TimeOn|1|0|61", "MagicAdd|a.esp", "MagicAdd||800",
                     "MagicAdd|a.esp|xyz", "MagicAdd|a/b.esp|800", "Cost|1|1", "CostPercent|101|0|0",
                     "Ragdoll|sideways|1", "Ragdoll|down|21", "FireArrow|0", "Sound|C:\\a.wav|head|1",
                     "Sound|..\\a.wav|head|1", "Sound|a.wav|Head|1", "Sound|a.wav|head|2", "SoundForm|a.esp|3C|head",
                     "GlobalSet|a.esp|D62|x", "ModEvent||t|1", "Strike", "CancelPair|1", "Nothing"})
                Check(!Parse(bad), (std::string("refused: '") + bad + "'").c_str());
        }
        // A window's span on a registered event (11-02): the registry's own, 0~60 seconds, windows only.
        {
            const auto p = Parse("<EventRegistry><Event name='Ghost'><Do op='iframeOn' duration='0.3'/></Event>"
                                 "<Event name='Slow'><Do op='timeOn' duration='60'/></Event></EventRegistry>");
            Check(p.ok && p.events[0].functions[0].span == 0.3f && p.events[0].functions[0].duration == 0.5f &&
                      p.events[1].functions[0].span == 60.0f,
                "a registered window's duration is its span");
            Check(Parse("<EventRegistry><Event name='Ghost'><Do op='iframeOn'/></Event></EventRegistry>")
                          .events[0]
                          .functions[0]
                          .span == 0.0f,
                "no duration: no span");
            Check(Rejected("<EventRegistry><Event name='G'><Do op='iframeOn' duration='61'/></Event></EventRegistry>"),
                "a span is at most 60 seconds");
            Check(
                Rejected("<EventRegistry><Event name='G'><Do op='iframeOff' duration='1'/></Event></EventRegistry>") &&
                    Rejected("<EventRegistry><Event name='G'><Do op='shake' duration='1'/></Event></EventRegistry>"),
                "the registry's duration is a window's on only");
            Check(Rejected("<Action id='A' clip='a.hkx'><EventValues><Event name='G'><Do op='iframeOn' duration='1'/>"
                           "</Event></EventValues></Action><EventRegistry><Event name='G'><Do op='iframeOn'/></Event>"
                           "</EventRegistry>"),
                "an action's values hold no span");
            Check(WithValues(p.events[0].functions[0], EventFunctionSpec{}).span == 0.3f,
                "the registry's span runs with any numbers");
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
        // A clip that turns a window on and off by itself owns it (11-02): the off event comes later in the clip.
        {
            using namespace ClipWindowsJudge;
            const Role ghostOn{0, 0b0001}, ghostOff{1, 0, 0b0001}, armorOff{2, 0, 0b0010}, both{3, 0b1000, 0b1000};
            const auto judge = [](std::vector<Note> notes) { return Judge(notes); };
            Check(judge({{0.1f, &ghostOn}, {0.4f, &ghostOff}}) == std::vector<Own>{{0, 0}},
                "on then off in one clip: the clip owns the window");
            Check(judge({{0.1f, &ghostOn}}).empty(), "on without off: another clip turns it off");
            Check(judge({{0.4f, &ghostOn}, {0.1f, &ghostOff}}).empty(), "an off before the on is another window's end");
            Check(judge({{0.1f, &ghostOn}, {0.4f, &armorOff}}).empty(), "another window's off does not count");
            Check(judge({{0.1f, &ghostOn}, {0.2f, &ghostOn}, {0.4f, &ghostOff}}) == std::vector<Own>{{0, 0}},
                "each pair once");
            Check(judge({{0.2f, &both}, {0.2f, &both}}).empty() && judge({{0.1f, &both}, {0.3f, &both}}).size() == 1,
                "one event turning a window on and off: a later one of it ends the window");
            // A paired clip's annotations are each actor's by prefix (11-02 review: a cut-short kill move with
            // NPCAI.IFrameOn and NPCAI.IFrameOff left the attacker immune).
            const auto part = [](std::string_view text, std::string_view body, int want) {
                const int got = PairPart(text);
                return got == want && text == body;
            };
            Check(part("NPCAI.IFrameOn", "AI.IFrameOn", 1) && part("2_dodge", "dodge", 2) && part("dodge", "dodge", 0),
                "a paired annotation's prefix names its actor and comes off");
            Check(part("NPC", "NPC", 0) && part("2_", "2_", 0), "a prefix alone is a name as written");
            Check(PartOfPrefix(nullptr) == 1 && PartOfPrefix("NPC") == 1 && PartOfPrefix("2_") == 2 &&
                      PartOfPrefix("") == 0 && PartOfPrefix("3_") == 0,
                "a synchronized clip's prefix: null is NPC, an unknown one reads annotations as written");
        }
        std::cout << checks << " event registry checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
