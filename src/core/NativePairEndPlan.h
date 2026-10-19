#pragma once
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

// The pure decisions of NativePairEnd (0.12.255): when a participant's pair begins to end, which clips are left
// alone, and the fixed table of participants handled at once. No engine types; tested without the game.
namespace AnimationInjector::NativePairEndPlan {
inline constexpr float kEndWindow = 0.1f;  // a single-play clip's last 0.1 s begins the end (as AI pairs)
inline constexpr float kBudget = 2.0f;     // seconds of handoff after the end begins (the AI session's default)
inline constexpr float kForget = 3.0f;     // a participant no graph has reported for this long is dropped
inline constexpr std::size_t kCapacity = 16;

// The end begins at the clip's deactivation, when the scene stops being synchronized (bIsSynced off), or in a
// single-play clip's last kEndWindow seconds. A looping clip's time wraps, so only the first two count for it.
inline bool EndBegins(float time, float duration, bool looping, bool deactivated, bool synced) {
    if (deactivated || !synced) return true;
    if (looping || !std::isfinite(time) || !std::isfinite(duration) || duration <= 0) return false;
    return time >= duration - kEndWindow;
}

// Mounting and dismounting are synchronized clips too: the engine seats the rider on the horse at their end, and
// moving the rider then would fight it. Their files name the horse (HorseEnter*, HorseExit*, *Horse*).
inline bool Excluded(std::string_view animation) {
    constexpr std::string_view horse = "horse";
    if (animation.size() < horse.size()) return false;
    for (std::size_t at = 0; at + horse.size() <= animation.size(); ++at) {
        std::size_t i = 0;
        while (i < horse.size() &&
               std::tolower(static_cast<unsigned char>(animation[at + i])) == static_cast<unsigned char>(horse[i]))
            ++i;
        if (i == horse.size()) return true;
    }
    return false;
}

// Participants by identity (an actor handle's native value): Find, or Claim a free slot for a new one. A full table
// claims nothing (the extra participant keeps the engine's own end). Identity 0 is never stored.
struct Slots {
    std::array<std::uint32_t, kCapacity> identity{};
    int Find(std::uint32_t id) const {
        if (!id) return -1;
        for (std::size_t i = 0; i < kCapacity; ++i)
            if (identity[i] == id) return static_cast<int>(i);
        return -1;
    }
    int Claim(std::uint32_t id) {
        if (!id) return -1;
        if (const int found = Find(id); found >= 0) return found;
        for (std::size_t i = 0; i < kCapacity; ++i)
            if (!identity[i]) {
                identity[i] = id;
                return static_cast<int>(i);
            }
        return -1;
    }
    void Release(int slot) {
        if (slot >= 0 && static_cast<std::size_t>(slot) < kCapacity) identity[static_cast<std::size_t>(slot)] = 0;
    }
};
}
