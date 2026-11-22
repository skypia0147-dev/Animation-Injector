#pragma once
#include <array>
#include <cstdint>
#include <string_view>

// gripHold / gripRelease settings (1.0.9), without engine headers (parser, checker and tests).
// A hand's attachment bone held at the value it had when gripHold fired: the right hand's Weapon bone, the left
// hand's Shield bone. The view says which of the actor's graphs hold it (third person, first person or both).
// A release lets go at once (no blend, user decision 2026-11-21).
namespace AnimationInjector {
enum class GripHand : std::uint8_t { kRight = 1, kLeft = 2, kBoth = 3 };
enum class GripView : std::uint8_t { kThird = 1, kFirst = 2, kBoth = 3 };

inline constexpr std::array<std::pair<std::string_view, GripHand>, 3> kGripHands{
    {{"right", GripHand::kRight}, {"left", GripHand::kLeft}, {"both", GripHand::kBoth}}};
inline constexpr std::array<std::pair<std::string_view, GripView>, 3> kGripViews{
    {{"third", GripView::kThird}, {"first", GripView::kFirst}, {"both", GripView::kBoth}}};

template<class T, std::size_t N>
constexpr bool ParseGrip(const std::array<std::pair<std::string_view, T>, N>& table, std::string_view text, T& out) {
    for (const auto& [name, value] : table)
        if (name == text) {
            out = value;
            return true;
        }
    return false;
}
constexpr bool Has(GripHand set, GripHand one) {
    return (static_cast<std::uint8_t>(set) & static_cast<std::uint8_t>(one)) != 0;
}
constexpr bool Has(GripView set, GripView one) {
    return (static_cast<std::uint8_t>(set) & static_cast<std::uint8_t>(one)) != 0;
}
}  // namespace AnimationInjector
