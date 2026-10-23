#pragma once
#include "PCH.h"
#include "runtime/ActionSpec.h"
#include "runtime/EventRegistrySpec.h"
#include <span>

// The global event registry (XML <EventRegistry>, format 3 on): event name -> functions, merged from
// every declaration file. Any actor whose graph sends a registered name runs its functions, in any
// animation (decision D1). Functions run through runtime/EventEffects.h.
namespace AnimationInjector::EventRegistry {
// Once, when the declarations are published (on the thread of their first reader); Observe and Update
// do nothing before.
// Every file's rules are merged: all functions of an event run (D3). A function equal to one
// already registered for that event is dropped; the same event and op with other settings is kept
// and logged as a conflict.
// A rule without functions only registers its name (a window, QTE or trigger event of an action).
// Each action's <EventValues> is resolved here to full functions by event and function index (0.12.281), so an
// event that arrives does no string work: it runs with the numbers of the action or pair playing on its sender,
// else the defaults. Unmatched values are logged and dropped.
void Build(const std::vector<EventRuleSpec>& rules, std::span<const ActionSpec> actions);

// Names to add to the graphs' event tables, so that clip annotations with these names
// are sent: every registered name. Empty when nothing is registered.
const std::vector<std::string>& Names();
// Registered event names, for logs and tests.
std::size_t Count();

// Any thread (the animation event hook, every actor). One lookup; a registered event is copied into a
// bounded queue with its sender. Nothing about the actor is changed here. Numbers an annotation carries
// are runtime/AnnotationCommands.h's ("AI.CamShake|1.5|0.5").
void Observe(RE::Actor* actor, const RE::BSFixedString& event);
// Any thread (the NotifyAnimationGraph hook): an event sent INTO an actor's graph starts the event's QTEs only
// (0.12.248; the engine sends Ragdoll this way). Its functions keep hearing only what the graph sends out.
void ObserveSent(RE::Actor* actor, const RE::BSFixedString& event);

// Main thread (player update): runs the queued events' functions (delayed ones when their game
// time is due), then EventEffects::Update.
void Update(float delta);

// Tells other plugins which event names are handled here for every actor (SKSE mod callback
// "AnimationInjector_EventOwner" without a sender). Sent after Build and again after each load.
void Announce();

// Load / new game: drops queued and delayed functions (EventEffects::ResetOnLoad ends the holds).
void ResetOnLoad();

// Main thread (ViewGuard, a cancelled run): drops the pending delayed functions of events `actor` sent
// or its pair partner sent while paired with it.
void DropDelayed(RE::ActorHandle actor);
}
