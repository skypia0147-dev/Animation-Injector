#pragma once
#include "PCH.h"

namespace AnimationInjector::FireArrow {
void Install();
void ResetOnLoad();
void ObserveArrowRelease(RE::Actor* actor);
std::uint64_t CurrentDraw();
void Release(RE::Actor* actor, std::uint64_t draw, const char* action, float elapsed);
}
