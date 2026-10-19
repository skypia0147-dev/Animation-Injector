#pragma once
#include "runtime/QteSpec.h"
#include <cstdint>
#include <string>
#include <vector>

// XML format 3 event registry, without engine headers (parser, checker and tests).
//
//   <EventRegistry>
//     <Event name="Grapple_Stom">
//       <Do op="spell" target="sender" plugin="FH_Grapple.esp" form="80B"/>
//     </Event>
//     <Event name="Grapple_CamShake" values="1.5|0.5 3.0|1.0">...</Event>
//   </EventRegistry>
//
// An event name means the same functions in every animation and every declaration file: any actor
// whose graph sends it runs them, whether or not one of our actions plays (the global event registry plan in docs/, 0.12.222).
namespace AnimationInjector {
enum class EventOp : std::uint8_t {
    kHide,  // weapon and shield nodes (lasting until kShow)
    kShow,
    kSpell,    // cast once
    kStateOn,  // add a spell (lasting until kStateOff)
    kStateOff,
    kCost,          // damage stamina / magicka once
    kShake,         // camera shake, player only
    kStopVelocity,  // clear the character controller's velocity once
    kTimeOn,        // global time multiplier, player only (lasting until kTimeOff)
    kTimeOff,
    kIFrameOn,  // ghost + invulnerable (lasting until kIFrameOff)
    kIFrameOff,
    kArmorOn,  // ignore stagger (lasting until kArmorOff)
    kArmorOff,
    kRotateOn,  // the graph's rotation variable (lasting until kRotateOff)
    kRotateOff,
    kCancel,     // ends the sender's pair
    kFireArrow,  // automatic shot of the drawn bow, player only
};
// The actors a function changes. Pair roles: kInitiator started the pair, kReceiver is the actor it was
// started on (the engine calls that one the "lead" of a paired idle, so the names avoid lead/support).
// kPartner, kInitiator and kReceiver need the sender to be in a pair and are skipped otherwise; kBoth is
// the sender and, in a pair, its partner.
enum class EventTarget : std::uint8_t { kSender, kPartner, kBoth, kPlayer, kInitiator, kReceiver };
// Whose event runs a function. kInitiator and kReceiver: the sender plays that role of a pair.
enum class EventFrom : std::uint8_t { kAny, kPlayer, kNpc, kInitiator, kReceiver };

// One <Do>. Settings an op does not read stay at their defaults.
struct EventFunctionSpec {
    EventOp op = EventOp::kSpell;
    EventTarget target = EventTarget::kSender;
    EventFrom from = EventFrom::kAny;
    float delay = 0.0f;                          // seconds of game time; cancelled by a load
    std::string plugin, form;                    // kSpell, kStateOn, kStateOff (form: local hex id)
    bool releasePhysics = false;                 // kSpell in a pair: first frees the pair's physics hold
    std::string space;                           // kSpell: only when this space direction measures
    float minSpace = 0.0f, maxSpace = 10000.0f;  // within [minSpace, maxSpace]
    float stamina = 0.0f, magicka = 0.0f;        // kCost
    float strength = 1.0f, duration = 0.5f;      // kShake
    bool parameters = false;                     // kShake: "name|strength|duration" overrides both
    float factor = 1.0f;                         // kTimeOn
    bool operator==(const EventFunctionSpec&) const = default;
};

// One <Event>: a name and its functions. No functions: the name is only registered in the graphs
// (an event an action uses for a window, a QTE or a trigger).
struct EventRuleSpec {
    std::string name;
    std::vector<EventFunctionSpec> functions;
    std::string source;  // declaration file, for logs
    // Payloads clips send after the name ("Grapple_CamShake|1.5|0.5" -> "1.5|0.5"). A clip annotation is
    // sent only when its whole text is an event name of the graph, so each "name|value" is registered too.
    std::vector<std::string> values;
    // QTEs the event starts for the player (0.12.248): no HKX, asked at once, kept through a ragdoll. They
    // hear the name sent into the player's graph (the engine's Ragdoll request) as well as sent out of it.
    std::vector<QteSpec> qtes;
};
// The names a rule puts in the graphs' event tables: its name, then "name|value" for each value.
inline std::vector<std::string> GraphNames(const EventRuleSpec& rule) {
    std::vector<std::string> out{rule.name};
    for (const auto& value : rule.values) out.push_back(rule.name + "|" + value);
    return out;
}

// A pair's clip-time effect (<Effects><Do at="seconds" .../>), run by the same executor.
struct TimedEffectSpec {
    float at = 0.0f;  // seconds from the pair clip's start
    EventFunctionSpec function;
};
}
