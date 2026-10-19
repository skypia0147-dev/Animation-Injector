#pragma once
#include "PCH.h"

// hkbStateMachine::handleEvent(vfunc 6), always installed: a retiring pair's IdleStop must not
// cut the linked action it handed off to (handoff guard).
// With bDebug it also logs events that reach our states while an action runs: whether they
// transition, and the delayed/active transition counts. An event repeated every frame is logged
// once per name; a transition, or a name containing "attack", every time.
namespace AnimationInjector::GraphEventHooks {
void Install();
}
