#pragma once

// How long the watch after a cancel-out lasts (ActionRunner::PostCancelWatch). An action cancelled into an attack
// leaves bAnimationDriven to that attack, and the watch lowers it once the attack is over if the attack did not.
// It used to end 5 seconds after the cancel whatever the actor was doing: an attack state held longer than that
// (a drawn bow, a long chain) outlived the watch, and a flag it then left stayed (2026-11-10 review D1).
namespace AnimationInjector::PostCancelTime {

constexpr float kWatch = 5.0f;   // the watch from the cancel-out, counted while the actor is not attacking
constexpr float kGrace = 1.0f;   // at least this much is left while an attack state is held, to see its end
constexpr float kLimit = 60.0f;  // an attack state held past this no longer extends the watch: it always ends

// The time left after a frame's countdown: `left` already has the frame taken off, `age` is the time since the
// cancel-out.
constexpr float Left(float left, float age, bool attacking) {
    return attacking && age < kLimit && left < kGrace ? kGrace : left;
}

static_assert(Left(0.2f, 10.0f, true) == kGrace, "an attack state still held keeps the watch");
static_assert(Left(4.0f, 1.0f, true) == 4.0f, "a watch with time left is not shortened");
static_assert(Left(0.2f, 10.0f, false) == 0.2f, "the watch counts down once the attack is over");
static_assert(Left(0.2f, kLimit, true) == 0.2f, "past the limit the watch ends even while attacking");

}  // namespace AnimationInjector::PostCancelTime
