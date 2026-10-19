#pragma once
#include "PCH.h"

// End position of the pairs the AI session does not run: vanilla kill moves and other mods' paired idles
// (0.12.255, the user's decision 2026-10-03: every pair, on by default; the per-pair XML EndPosition is gone).
// A synchronized clip whose root bone walked away from the actor snaps the actor back to where the scene started
// as the clip blends out; the disappearing horizontal root offset is handed to the actor instead, with the AI
// pairs' calculation (PairRootHandoff: horizontal only, flat ground; stops on death, ragdoll, knockdown, falling or
// an outside move, so a kill move's victim is left alone). Mounting and dismounting are left to the engine.
namespace AnimationInjector::NativePairEnd {
// [Compatibility] iPairEndPosition: 0 off, 1 Animation Injector pairs only, 2 every paired animation.
enum class Scope : std::uint8_t { kOff, kAiPairs, kEvery };
Scope Configured();
void Install();
// Graph threads (ClipHooks::ObservePair, for a pair the AI session does not own): a synchronized clip of `actor`
// activated, updated or deactivated. Copies values only; never retains an engine pointer.
void ObserveClip(
    RE::Actor* actor, const RE::hkbContext& context, const RE::BSSynchronizedClipGenerator& clip, bool exiting);
// Main thread, after the animation update. A participant's end begins at the clip's last 0.1 s, its deactivation
// or the scene's end (bIsSynced off); up to 16 participants at once.
void Update(float delta);
// Save load / new game: drops the bookkeeping, never moves anyone back.
void Reset();
}
