#pragma once
#include "runtime/QteSpec.h"

// What a converging-lines QTE's result does outside its own drawing (0.12.246): the camera shakes, and a miss drains
// the world's colour for a moment (ui/QteConverge.h Desat). Main thread only (Qte::Update).
namespace AnimationInjector::QteScreenFx {
// reducedMotion keeps the camera still (the drain still plays).
void Result(QteOutcome outcome, bool reducedMotion);
void Update(float delta);
// Gives the image space back at once (load, new game).
void ResetOnLoad();
}
