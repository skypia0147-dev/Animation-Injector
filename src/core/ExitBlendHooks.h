#pragma once
#include "PCH.h"

namespace AnimationInjector::ExitBlendHooks {
// Record an owned pair exit for its later destination activation.
void CaptureDestination(RE::hkbStateMachine* root, const RE::hkbContext& context, const RE::hkbEvent& event);
// Register only explicitly authored normal exits, never entry or link effects (a pair's loss exit: below).
void Register(RE::hkReferencedObject* effect, float duration, int action);
// The normal exit effect shared by every pair of one role (core/PairEntry.h). Its action is the pair
// the actor plays, and it counts only when that pair authored an exit blend, as with Register.
void RegisterSharedPairExit(RE::hkReferencedObject* effect);
// The partner-loss exit effect shared by every pair of one role (0.12.286): the survivor's blend when its partner
// dies or ragdolls. Its action is the pair the actor plays, and it counts only when that pair authored a loss blend
// (OnPartnerLost); move input during it hands the survivor's movement back, the blend kept, as a normal exit does.
void RegisterSharedPairLoss(RE::hkReferencedObject* effect);
// The exit, recovery and partner-loss events of a pair with an authored exit or loss blend that uses the shared exit
// effects (Register records them for a pair's own effect).
void RegisterPairDestination(int action);
void Drain();
void Reset();
void SetMoveInput(bool held);
void Tick(float delta);
void Install();
}
