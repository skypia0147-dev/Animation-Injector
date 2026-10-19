#pragma once
#include "RE/B/BSPointerHandle.h"

namespace AnimationInjector::PrecisionLink {
// Precision (ersh1) turns a hit actor's whole ragdoll dynamic and drives it ("active ragdoll") until its
// blend-out, which can outlast a grab: a carry then found every body simulated by someone else and let the
// target go (0.12.242). From the lift's start until the carry ends the target is out of Precision
// (IVPrecision3::ToggleDisableActor): Precision removes its ragdoll and takes no hits on it. Main thread
// only, driven by Carry. Without Precision.dll every call does nothing.

// The lift starts (Carry::PairStarted).
void Hold(RE::ActorHandle target);
// The carry ended, any reason (Carry::Stop): Precision gets back only an actor Hold took out.
void Release(RE::ActorHandle target);
// Load / new game: Precision keeps its list across loads.
void ReleaseAll();
}
