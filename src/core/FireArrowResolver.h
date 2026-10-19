#pragma once
#include <cstdint>

namespace AnimationInjector::FireArrowResolver {
using CodeRange = bool (*)(std::uintptr_t start, std::uintptr_t& end);
struct Route {
    std::uintptr_t pressCall = 0, releaseCall = 0, dispatch = 0, finishCharge = 0;
    explicit operator bool() const { return pressCall && releaseCall && dispatch && finishCharge; }
};
// Start from the Address Library AttackBlockHandler::ProcessButton, and accept
// only the native right-hand action sequence with both matching call targets.
Route Resolve(std::uintptr_t processButton, CodeRange range);
bool Validate(const Route& route, std::uintptr_t sourceActionMap, CodeRange range);
}
