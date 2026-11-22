#pragma once
#include <cstdint>
#include <vector>
// Records the key presses the input sink hands to the ladder toggle (core/LocomotionToggle.h).
namespace AnimationInjector::LocomotionToggle {
inline std::vector<std::uint32_t> presses;
inline void OnKeyDown(std::uint32_t code) { presses.push_back(code); }
}
