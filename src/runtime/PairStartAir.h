#pragma once

namespace AnimationInjector::PairStartAir {
// A pair exit re-arms the character controller in air; it lands on its next simulated
// steps. A pair holds the controller unsimulated, so a pair that starts inside that
// window keeps the in-air state for its whole playback although nobody left the ground.
inline constexpr float kWindow = 1.0f;

struct Exit {
    bool grounded = false;  // the ended pair started on the ground, directly or by inheritance
    float age = kWindow + 1.0f;
};

// In air at start only because the same actor's previous pair has just ended.
inline bool Inherited(bool air, bool sameActor, const Exit& exit) {
    return air && sameActor && exit.grounded && exit.age <= kWindow;
}
}
