#pragma once
#include <cstdint>

namespace AnimationInjector::CarryPoseLock {
// The carried target's animated pose, locked when the lift ends, or earlier at the carry's fix moment.
//
// Between the lift pair's exit and the carry's attach, the target's own exit blend used to return
// it to its standing pose, and the carry (and its physics) started from that pose instead of the
// authored lift end (2026-09-25: game ragdoll traces start at the standing pose's bone distances).
// Once locked, every clip output of the target's character is the locked lift output, so blends
// toward other states see the same pose, until the carry ends.
//
// A fix moment (CarrySpec::fixAt) locks the pose while the lift still plays; the carry then moves the
// locked pose's root bone every frame so the target stays on the carrier bone (Follow). The root is
// written into the animation output: a scene placement written from the main update during a pair
// is overwritten before it is drawn (0.12.161).
//
// hkbClipGenerator::Generate exit, installed after PairRootScale so it sees the scale-corrected
// root. The hook reads identities only (no dereference) and never takes a lock.
void Install();
// The root bone's local transform (pose track bone 0): translation in its parent's space,
// rotation in Havok order (x, y, z, w).
struct RootLocal {
    float translation[3];
    float rotation[4];
};
// Animation thread (Carry::ObserveClip, target role): the target's character and its lift clip.
// The lift clip's own output (never a blend) is kept, one frame deep, until the lock. With fixAt >= 0
// the lock takes the first lift output at or past fixAt seconds of clip time; otherwise the lift's end.
void Watch(std::uintptr_t character, std::uintptr_t liftClip, float fixAt);
// Any thread, idempotent: the lift is ending. Lock the last pure lift output, or the next one.
void CaptureNow();
// Once captured, every clip output of the watched character is the captured pose.
bool Captured();
// Clip time of a lock taken at the fix moment; < 0 when not locked or locked by the lift's end.
float FixedAt();
// Main thread, every frame while fixed during the lift: written over the locked pose's root bone.
// The last one stays until Release, so the lift's end and the attach see no jump.
void Follow(const RootLocal& root);
// Main thread: the target's 3D was replaced (cell transfer): keep the pose, follow the new character.
void Rebind(std::uintptr_t character);
// Main thread: the carry ended (stop, drop, putdown release, load). Outputs return to the graph.
void Release(const char* reason);
}
