#pragma once
#include "PCH.h"

// An actor's attack data (HighProcessData::attackData) while we change it (1.1.0). The engine (damage multiplier,
// stagger, power attack, which hand strikes) and Precision (the collision's weapon node) read "what attack is this"
// from it. Two of ours name it: the action playing on the actor (<Action attack>) and an event function
// (attackDataOn / attackDataOff, AI.AttackDataOn|name / AI.AttackDataOff), so a clip an animation replacer swapped in
// can name its own (a left-hand swing in one moveset). The rules are runtime/AttackDataSlots.h: the event's is over
// the action's, the value from before comes back when both are gone, and a value the engine replaced meanwhile is
// left alone.
//
// Main thread. Entries are kept by FormID with references to the attack data only (no actor pointer).
namespace AnimationInjector::AttackDataLayer {
// The attack data `name` of the actor's race; null (and a warning naming the ones it has) when there is none.
RE::BGSAttackData* Find(RE::Actor* actor, const std::string& name, const char* who);

// The action `owner` (its index) playing on the actor names `data`; ClearAction when it ends.
void SetAction(RE::Actor* actor, int owner, RE::BGSAttackData* data);
void ClearAction(RE::Actor* actor, int owner);
// An event function names the race's attack data `name`. False when the actor has no such attack data.
bool SetEvent(RE::Actor* actor, const std::string& name, const char* who);
void ClearEvent(RE::Actor* actor);
// The event's attack data is still what the engine has (false once the engine replaced it: its hold is over).
bool EventInEffect(RE::Actor* actor);

// Load / new game: nothing of the old game is given back.
void Reset();
}
