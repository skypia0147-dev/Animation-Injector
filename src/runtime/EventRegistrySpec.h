#pragma once
#include "runtime/QteSpec.h"
#include "runtime/FireArrowShot.h"
#include "runtime/RagdollPush.h"
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
    kFireArrow,  // automatic shot of the bow (drawn, or direct at full power), player only
    kRagdoll,    // push into the engine's ragdoll once; a pair's role first leaves the pair's physics hold
};
// The actors a function changes. A registered event's function changes its sender (kSender, 0.12.273). A pair's
// clip-time effect names a role (0.12.275): kInitiator started the pair, kReceiver is the actor it was started on
// (the engine calls that one the "lead" of a paired idle, so the names avoid lead/support), kBoth is both and
// kPlayer the player's role. An ordinary action's clip-time effect changes the actor playing it.
enum class EventTarget : std::uint8_t { kSender, kBoth, kPlayer, kInitiator, kReceiver };

// One <Do>. Settings an op does not read stay at their defaults. A registry function holds what it acts with
// (a spell's plugin and form, a ragdoll's direction); the numbers it runs with come from the <EventValues> of the
// action or pair playing on the sender, else its defaults (0.12.281).
struct EventFunctionSpec {
    EventOp op = EventOp::kSpell;
    // A registry function always changes the event's sender (0.12.273); only a pair's clip-time effect sets a
    // target (initiator, receiver, both or player, 0.12.275).
    EventTarget target = EventTarget::kSender;
    float delay = 0.0f;        // seconds of game time; cancelled by a load
    std::string plugin, form;  // kSpell, kStateOn, kStateOff (form: local hex id)
    RagdollPush::Direction direction = RagdollPush::Direction::kDown;  // kRagdoll
    float force = -1.0f;                                               // kRagdoll: <0 = the direction's default
    float stamina = 0.0f, magicka = 0.0f;                              // kCost
    float strength = 1.0f, duration = 0.5f;  // kShake (an annotation's own numbers: AI.CamShake)
    float factor = 1.0f;                     // kTimeOn
    FireArrowShot arrow;                     // kFireArrow: drawn or direct, the direct shot's damage
    bool operator==(const EventFunctionSpec&) const = default;
};

// Ops that run only with an action's numbers (0.12.281): skipped, and logged, when the sender's action gives none.
constexpr bool NeedsValues(EventOp op) { return op == EventOp::kCost || op == EventOp::kTimeOn; }

// `registry` (op, target, plugin, form, direction) run with `values`' delay and numbers.
inline EventFunctionSpec WithValues(const EventFunctionSpec& registry, const EventFunctionSpec& values) {
    EventFunctionSpec out = values;
    out.op = registry.op;
    out.target = registry.target;
    out.plugin = registry.plugin;
    out.form = registry.form;
    out.direction = registry.direction;
    return out;
}

// One <Event> of an action's or pair's <EventValues> (0.12.281): the numbers the registry's functions of `event`
// take while it plays on the actor sending the event.
struct EventValuesSpec {
    std::string event;
    std::vector<EventFunctionSpec> functions;  // op, delay and numbers only
};

// One <Event>: a name and its functions. No functions: the name is only registered in the graphs
// (an event an action uses for a window, a QTE or a trigger).
struct EventRuleSpec {
    std::string name;
    std::vector<EventFunctionSpec> functions;
    std::string source;  // declaration file, for logs
    // QTEs the event starts for the player (0.12.248): no HKX, asked at once, kept through a ragdoll. They
    // hear the name sent into the player's graph (the engine's Ragdoll request) as well as sent out of it.
    std::vector<QteSpec> qtes;
};
// A clip-time effect (<Effects><Do at="seconds" .../>), run by the same executor: a pair's on the roles it
// names, an ordinary action's (0.12.274) on the actor playing it.
struct TimedEffectSpec {
    float at = 0.0f;  // seconds from the clip's start
    EventFunctionSpec function;
};
}
