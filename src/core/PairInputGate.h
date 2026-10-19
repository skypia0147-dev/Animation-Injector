#pragma once
#include <cstdint>

namespace AnimationInjector::PairInputGate {
// Intercept input and motion queries only; the native sync scene stays alive.
void Install();
// Resolve a unique call using decoded operands, not runtime-specific byte offsets.
std::uintptr_t FindCall(std::uintptr_t begin, std::uintptr_t end, std::uintptr_t managerGlobal);
// Resolve the bIsSynced virtual read in Actor's movement calculation.
std::uintptr_t FindMotionCall(
    std::uintptr_t begin, std::uintptr_t end, std::uintptr_t fixedStringsGetter, std::uint32_t syncedMember);
}
