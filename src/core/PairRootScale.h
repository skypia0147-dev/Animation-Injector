#pragma once

namespace AnimationInjector::PairRootScale {
// Paired animations are authored for scale-1 actors. An actor's 3D scale (reference scale x
// NPC height; vanilla EncChicken is 1.3) also multiplies the skeleton root's animated travel,
// so a scaled participant overshoots its partner and the pose no longer meets the other actor.
// While an Animation Injector pair holds a participant whose scale is not 1, the root bone's
// translation in every clip output of that character is divided by the scale its parent
// applies (hkbClipGenerator::Generate exit), so blends and the pose copy all see authored
// travel. 0.12.169 tried the end of Character::UpdateAnimation: NPCs never passed there.
// Body proportions keep the actor's real size; vanilla paired scenes are untouched.
void Install();
// Main thread, once per frame: follow PairedSession's participants.
void Update();
// Load / new game.
void Reset();
}
