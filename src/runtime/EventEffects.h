#pragma once
#include "PCH.h"
#include "runtime/EventRegistrySpec.h"

// The one executor of event functions: the global event registry (runtime/EventRegistry.h) and a
// pair's clip-time effects both run them here (docs/계획-전역이벤트등록.md §4.2-4.4).
//
// Lasting functions (hide, magic add, timeOn, iframeOn, armorOn, lock, rotateOn) take a hold on the
// target actor. A hold ends by, whichever comes first:
//   - its off function (show, magic remove, ...) on that actor: ends every hold of that kind there that
//     an event function took, whichever event or scope took it;
//   - the end of the run scope it was taken in (below), except a window (iframeOn, armorOn, rotateOn,
//     timeOn) a registered event turned on while some event is registered to turn it off: that one waits for
//     its off event, through the end of the animation (user decision 11-01);
//   - for such a window turned on from a clip that sends its off event later itself (runtime/ClipWindows.h):
//     the end of that clip, when it plays through or is cut short, as a clip range (user decision 11-02);
//   - for a window turned on with a span (a registered function's duration, an AI. command's last value): the
//     span's end, and only that besides its off and the common ends (11-02);
//   - the actor's death, ragdoll or unloaded 3D, or a load / new game.
// There is no time limit (decision D4). A hold taken outside any run scope lasts until its off
// function or one of the common ends.
//
// Holds share one ledger per kind with the other systems that change the same state (a carry's weapon
// cover, a clip's windows): the state returns to what it was before the first holder only when its
// last holder lets go (as runtime/WeaponCull.h does for weapon nodes).
namespace AnimationInjector::EventEffects {
// A run scope: an actor's stretch of our playback, from the first action or pair it starts with none
// open until it has had no running action, no pair and no pending link transition for a moment.
// Actions a link chains (Connect, handoff) stay in one scope, so a weapon a pair hid stays hidden in
// the action it hands over to until that action shows it or the chain ends. 0 = no scope.
using Scope = std::uint64_t;

// Main thread. The actor's open scope, opening one when a run has started and none is open; 0 when
// none of our actions or pairs plays on it (an animation we do not manage).
Scope ScopeOf(RE::Actor* actor);

struct Context {
    RE::ActorHandle sender;   // the actor whose graph sent the event
    RE::ActorHandle partner;  // its pair partner, empty outside a pair
    int senderRole = -1;      // 0 initiator, 1 receiver, -1 not in a pair
    bool senderIsPlayer = false;
    const char* origin = "";  // event name or "pair clip time", for logs and the automatic shot
    bool registered = false;  // a registered event's function (not a clip time's)
    std::uint64_t clip = 0;   // a window's: the sending clip that also turns it off (runtime/ClipWindows.h)
    bool command = false;     // an AI. annotation command's: its off is always there to send
};
// Main thread. The context of an event `sender` sent now (its pair role and partner, if any).
Context ContextFor(RE::Actor* sender, const char* origin);

// Main thread. Runs `function` for each of its targets (`target` against the context) that is loaded,
// alive and not ragdolled. A hold belongs to the target's run scope.
void Run(const EventFunctionSpec& function, const Context& context);
// Main thread. The same for targets chosen by the caller (a pair's clip-time effects).
void RunFor(const EventFunctionSpec& function, const Context& context, const std::vector<RE::ActorHandle>& targets);

// Main thread: the windows the clips that stopped turned on end (runtime/ClipWindows.h).
void EndClips(const std::vector<std::uint64_t>& clips);
// Main thread, every frame: closes scopes whose actor stopped playing (ending their holds), ends the
// holds of dead, ragdolled or unloaded actors and keeps held weapons hidden (a weapon drawn since gets
// a new node). Only actors with holds or open scopes are read.
void Update(float delta);
// Any thread: an armorOn hold ignores stagger (read with the clip windows by the notify hook).
bool HyperArmorFor(RE::ActorHandle actor);

// Load / new game: forgets every hold (the nodes belong to the old 3D), restores time and the ghost
// leases. State spells are in the save: see Save / PostLoad.
void ResetOnLoad();
// SKSE co-save: the spells a magic add still holds, as (actor, spell) form ids.
void Save(SKSE::SerializationInterface* serialization);
// A record of this module: read it and return true; any other record: false.
bool LoadRecord(
    SKSE::SerializationInterface* serialization, std::uint32_t type, std::uint32_t version, std::uint32_t length);
// After a load: removes the state spells the save recorded as held (their holds ended with the save).
void PostLoad();
}
