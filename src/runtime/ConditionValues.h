#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace AnimationInjector::ConditionValues {
inline bool Compare(double actual, std::string_view op, double value, double upper) {
    if (!std::isfinite(actual)) return false;
    if (op == "lt") return actual < value;
    if (op == "le") return actual <= value;
    if (op == "eq") return actual == value;
    if (op == "ge") return actual >= value;
    if (op == "gt") return actual > value;
    return op == "range" && actual >= value && actual <= upper;
}
inline std::uint64_t Mix(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
// Owned by an execution request; copying a queued request preserves its rolls.
// No actors, global result cache, wall-clock timer, or per-frame random rerolls.
struct Request {
    std::uint64_t seed = 0;
    // Snapshot input once per flow request, including queued availability checks.
    int movementDirection = -1;
    bool movementCaptured = false;
    void CaptureMovement(int direction) {
        if (movementCaptured) return;
        movementDirection = direction;
        movementCaptured = true;
    }
    // Charge (10-30): how long the key that started the player's action was held (HeldKeys::ChainHeldTime), once
    // per request; <0 without one (an NPC, an event start).
    float heldTime = -1.0f;
    bool heldCaptured = false;
    void CaptureHeld(float seconds) {
        if (heldCaptured) return;
        heldTime = seconds;
        heldCaptured = true;
    }
    // An event trigger's actor as it was when the event arrived: the engine sends SprintStop
    // while the sprint flag is still set and clears it before the queued request is resolved.
    bool sprinting = false;
    bool sprintingCaptured = false;
    void CaptureSprinting(bool value) {
        if (sprintingCaptured) return;
        sprinting = value;
        sprintingCaptured = true;
    }
    void Ensure() {
        static std::atomic_uint64_t sequence{1};
        if (!seed) seed = Mix(sequence.fetch_add(1, std::memory_order_relaxed));
    }
    bool Chance(int flow, int predicate, double percent) {
        Ensure();
        if (percent <= 0) return false;
        if (percent >= 100) return true;
        const auto bits = Mix(
            seed ^ Mix(static_cast<std::uint64_t>(flow)) ^ Mix(static_cast<std::uint64_t>(predicate) + 0x100000000ULL));
        return static_cast<double>(bits >> 11) * (100.0 / 9007199254740992.0) < percent;
    }
};
}
