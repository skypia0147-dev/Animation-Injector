#pragma once
#include "PCH.h"

namespace AnimationInjector::FireArrow {
// The bow shot (fireArrow, 0.12.303 its only way): no draw needed; full power (a full draw's speed and range)
// toward the crosshair, damage multiplied by `damage`, one arrow consumed. Player only. It needs no hook.
void Direct(RE::Actor* actor, float damage, const char* action, float elapsed);
}
