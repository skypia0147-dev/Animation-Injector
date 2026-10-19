#pragma once
#include <RE/A/ActorState.h>
#include <utility>

namespace AnimationInjector {
// FullBodyPlayerIdles can clear weaponState while the bow remains drawn.
inline bool FireArrowDrawReady(RE::ATTACK_STATE_ENUM state) {
    return state == RE::ATTACK_STATE_ENUM::kBowAttached || state == RE::ATTACK_STATE_ENUM::kBowDrawn;
}

template<class Actor>
auto FireArrowEquipment(Actor& actor) {
    return std::pair{actor.GetEquippedObject(true), actor.GetEquippedObject(false)};
}

struct FireArrowResult {
    bool accepted = false, completed = false;
};

// Input release closes charge; it is not the arrow event. Manual release has
// already executed the first two operations in the original input handler.
template<class Engine>
FireArrowResult CompleteFireArrow(bool automatic, Engine& engine) {
    bool accepted = true;
    if (automatic) {
        accepted = engine.ReleaseAction();
        engine.CloseCharge();
    }
    if (engine.Available() && !engine.ArrowReleased()) engine.DeliverArrowEvent();
    return {accepted, engine.Available() && engine.ArrowReleased()};
}
}
