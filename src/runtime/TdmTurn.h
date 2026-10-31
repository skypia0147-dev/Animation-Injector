#pragma once
#include "runtime/TdmMovement.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace AnimationInjector::TdmTurn {
// An angle in [-pi, pi].
inline float Wrap(float a) { return std::remainder(a, 2 * std::numbers::pi_v<float>); }

// Turning the player in TDM's stead (10-31). TDM ignores movement input while the player is animation driven
// (DirectionalMovementHandler::ProcessInput: "Skip if player is animation driven"), and our actions are: so in an
// action's Rotate window nothing turned the player under TDM's free camera, whatever the keys (user report 10-31).
// There the runtime turns the player as TDM's setting would while a movement key is held: vanilla style faces the
// camera's front (the crosshair), directional faces the keys' way from the camera. No movement input, TDM absent,
// off or locked on a target (TDM turns to its target itself): no turn.

// The heading to turn toward (radians, [-pi, pi]), from TDM's setting, the camera's yaw and the movement input
// (x right, y forward; the game's heading turns clockwise). NaN: no turn.
inline float Target(TdmMovement::Mode mode, float cameraYaw, float inputX, float inputY) {
    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
    if (std::fabs(inputX) < 0.25f && std::fabs(inputY) < 0.25f) return kNaN;
    float h;
    if (mode == TdmMovement::Mode::kVanillaStyle)
        h = cameraYaw;
    else if (mode == TdmMovement::Mode::kDirectional)
        h = cameraYaw + std::atan2(inputX, inputY);
    else
        return kNaN;
    return Wrap(h);
}

// This frame's share of the turn still to make: eased in at 12 per second (about TDM's own camera easing), the whole
// rest once the frame would pass it.
inline float Step(float remaining, float delta) { return remaining * std::clamp(delta * 12.0f, 0.0f, 1.0f); }

// The engine moves a clip's root motion (AMR) along a heading of its own, which turning the body by SetHeading does not
// turn: it stays across loop cycles, clips, actions and standing still, and only the engine's own turning and walking
// bring it along (logs 10-31: heading 1.496 -> 3.597 with the motion still at 1.496; a later action starting at 4.89
// moving at 1.10). So the player's clip translation (x right, y forward) turns by a steer, heading minus the engine's.

// The translation turned by the steer (as the heading turns: clockwise for a positive angle).
inline void Steer(float steer, float& x, float& y) {
    const float s = std::sin(steer), c = std::cos(steer), x0 = x;
    x = x0 * c + y * s;
    y = -x0 * s + y * c;
}

// The engine's motion heading is what it moved minus the way of the (steered) translation it was given; the steer that
// moves the clip along the heading is the heading minus it.
inline float SteerFor(float heading, float velocityYaw, float steeredClipYaw) {
    return Wrap(heading - (velocityYaw - steeredClipYaw));
}

// A measurement to trust: both moving, at speeds that agree (a wall or a slope changes what the engine moved).
inline bool Comparable(float engineSpeed, float clipSpeed) {
    return engineSpeed > 30.0f && clipSpeed > 30.0f && std::fabs(engineSpeed / clipSpeed - 1.0f) < 0.25f;
}
}  // namespace AnimationInjector::TdmTurn
