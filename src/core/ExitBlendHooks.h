#pragma once
#include "PCH.h"

namespace AnimationInjector::ExitBlendHooks {
// Record an owned pair exit for its later destination activation.
void CaptureDestination(RE::hkbStateMachine* root, const RE::hkbContext& context, const RE::hkbEvent& event);
// Register only explicitly authored normal exits, never entry/link/loss effects.
void Register(RE::hkReferencedObject* effect, float duration, int action);
// The normal exit effect shared by every pair of one role (core/PairEntry.h). Its action is the pair
// the actor plays, and it counts only when that pair authored an exit blend, as with Register.
void RegisterSharedPairExit(RE::hkReferencedObject* effect);
// The exit and recovery events of a pair with an authored exit blend that uses the shared exit effect
// (Register records them for a pair's own effect).
void RegisterPairDestination(int action);
void Drain();
void Reset();
void SetMoveInput(bool held);
void Tick(float delta);
void Install();
}
