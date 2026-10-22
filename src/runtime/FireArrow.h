#pragma once
#include "PCH.h"

namespace AnimationInjector::FireArrow {
void Install();
void ResetOnLoad();
void ObserveArrowRelease(RE::Actor* actor);
std::uint64_t CurrentDraw();
void Release(RE::Actor* actor, std::uint64_t draw, const char* action, float elapsed);
// A direct shot (mode="direct"): no draw needed; full power (a full draw's speed and range), damage multiplied by
// `damage`, one arrow consumed. Player only.
void Direct(RE::Actor* actor, float damage, const char* action, float elapsed);
}
