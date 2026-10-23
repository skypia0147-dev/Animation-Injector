#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

// The ragdoll function (0.12.278): pushes an actor into the engine's ragdoll from a point beside it, as
// FH_Grapple.esp's GrapplePush did with a marker and PushActorAway, without the spell, the marker or Papyrus.
namespace AnimationInjector::RagdollPush {
enum class Direction : std::uint8_t { kDown, kUp, kForward, kBackward, kLeft, kRight };
inline constexpr std::array<std::string_view, 6> kNames{"down", "up", "forward", "backward", "left", "right"};

// The push point in the actor's own frame (game units) and the default force; the actor flies away from it.
struct Preset {
    float forward, right, up, force;
};
inline constexpr std::array<Preset, 6> kPresets{{
    {0, 0, 160, 1},   // down
    {0, 0, -50, 2},   // up
    {-50, 0, 50, 5},  // forward (point behind)
    {50, 0, 50, 5},   // backward
    {0, 50, 50, 5},   // left (point on the right)
    {0, -50, 50, 5},  // right
}};
inline constexpr float kMaxForce = 20.0f;

struct Point {
    float x, y, z;
};
// The world push point for an actor at (x, y, z) facing yaw (Skyrim's angle Z: 0 = +Y, clockwise), the
// offsets GrapplePush turned with the target's facing.
inline Point PushPoint(Direction direction, float x, float y, float z, float yaw) {
    const auto& p = kPresets[static_cast<std::size_t>(direction)];
    const float s = std::sin(yaw), c = std::cos(yaw);
    return {x + p.forward * s + p.right * c, y + p.forward * c - p.right * s, z + p.up};
}
// force < 0 = the direction's default.
inline float ForceOf(Direction direction, float force) {
    return force < 0.0f ? kPresets[static_cast<std::size_t>(direction)].force : std::min(force, kMaxForce);
}
}
