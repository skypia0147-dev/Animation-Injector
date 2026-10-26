#pragma once
#include <cstdint>
#include <string_view>

// A game control (0.12.292): an input given by what the game calls it - control="jump" - instead of a key. The
// game maps it to the player's keyboard, mouse and gamepad bindings (ControlMap), so one choice covers every
// device and follows the player's remapping. Names are the members of RE::UserEvents.
namespace AnimationInjector {

enum class GameControl : std::uint8_t {
    kNone,
    kJump,
    kSprint,
    kSneak,
    kActivate,
    kReadyWeapon,
    kShout,
    kRightAttack,
    kLeftAttack,
    kTogglePOV,
    kRun,
    kToggleRun,
    kAutoMove,
    kWait,
    kFavorites,
    kTweenMenu,
};

// The XML names, in the order of the enum (kNone has none).
inline constexpr std::string_view kGameControlNames[] = {"", "jump", "sprint", "sneak", "activate", "readyWeapon",
    "shout", "rightAttack", "leftAttack", "togglePOV", "run", "toggleRun", "autoMove", "wait", "favorites",
    "tweenMenu"};
inline constexpr int kGameControlCount = static_cast<int>(std::size(kGameControlNames));

// kNone for an empty or unknown name (exact case, as the XML writes it).
constexpr GameControl ParseGameControl(std::string_view name) {
    for (int i = 1; i < kGameControlCount; ++i)
        if (kGameControlNames[i] == name) return static_cast<GameControl>(i);
    return GameControl::kNone;
}
constexpr std::string_view GameControlName(GameControl control) { return kGameControlNames[static_cast<int>(control)]; }

}  // namespace AnimationInjector
