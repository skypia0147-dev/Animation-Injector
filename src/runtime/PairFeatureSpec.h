#pragma once
#include "runtime/EventRegistrySpec.h"
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
    float openAt = 0, closeAt = -1;
    bool buffered = true;
    bool closeRelativeToEnd = false;
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
