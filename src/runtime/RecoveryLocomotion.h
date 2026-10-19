#pragma once
#include <cmath>

namespace AnimationInjector {
// The native action filter rejects moveStart/moveStop while a sync scene is
// registered. Reconcile only locomotion edges during an owned movement exit.
// This state does not own keys, velocity, or the animation graph itself.
struct RecoveryLocomotion {
    bool moving = false;

    template<class Send>
    void Update(bool eligible, bool readable, float speed, bool moveIntent, Send&& send) {
        if (!eligible || !readable || !std::isfinite(speed) || speed < 0) return;
        // Prepare locomotion while the controller is still accelerating from
        // the paired pose. Zero speed with held input must not undo that start.
        // On release, let physical deceleration finish before requesting idle.
        const bool next = moveIntent || speed > 0.01f;
        if (next != moving && send(next)) moving = next;
    }
};
}
