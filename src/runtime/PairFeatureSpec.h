#pragma once
#include "runtime/EventRegistrySpec.h"
#include "runtime/GameControls.h"
#include <string>
#include <vector>

namespace AnimationInjector {
struct ActionLinkSpec {
    std::string action, event, openEvent, closeEvent;
    // Optional ordinary action for the existing paired partner.
    std::string partnerAction;
    std::string enterEvent, airEnterEvent;
    // Internal immutable routes for a conditional ordinary destination.
    int flowParent = -1;
    float blend = -1.0f;
    unsigned key = 0, gamepad = 0;
    GameControl control = GameControl::kNone;  // a game control instead of a key (0.12.292)
    float openAt = 0, closeAt = -1;
    bool buffered = true;
    bool closeRelativeToEnd = false;
    // Charge (10-30): on="end" goes at the source's end (its Exit point) instead of exiting; on="release" goes once
    // its key is up while the window is open (key, gamepad, control; none: the key that started the action).
    bool onEnd = false, onRelease = false;
    // on="move" (10-31): goes on a movement key's press as a key transition on its key's, buffered too; a movement
    // key held as the window opens counts as buffered. In a charge only (the key that started the action still
    // down), the player only.
    bool onMove = false;
};
struct PairFeatureSpec {
    bool enabled = false;
    std::string cameraReset;
    bool cameraRestoreOnEnd = true;
    float cameraTimeout = 10.0f;
    bool interruptOnThirdPartyHit = false;
    // Clip-time effects (<Effects><Do at="seconds">). Event effects are in the global event registry.
    std::vector<TimedEffectSpec> timed;
};
}
