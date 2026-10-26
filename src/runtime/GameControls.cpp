#include "PCH.h"
#include "runtime/GameControlBindings.h"

namespace AnimationInjector::GameControls {
namespace {
// The game's user event for each control, in the enum's order (nullptr for kNone).
const RE::BSFixedString* UserEvent(GameControl control) {
    auto* e = RE::UserEvents::GetSingleton();
    if (!e) return nullptr;
    switch (control) {
    case GameControl::kJump: return &e->jump;
    case GameControl::kSprint: return &e->sprint;
    case GameControl::kSneak: return &e->sneak;
    case GameControl::kActivate: return &e->activate;
    case GameControl::kReadyWeapon: return &e->readyWeapon;
    case GameControl::kShout: return &e->shout;
    case GameControl::kRightAttack: return &e->rightAttack;
    case GameControl::kLeftAttack: return &e->leftAttack;
    case GameControl::kTogglePOV: return &e->togglePOV;
    case GameControl::kRun: return &e->run;
    case GameControl::kToggleRun: return &e->toggleRun;
    case GameControl::kAutoMove: return &e->autoMove;
    case GameControl::kWait: return &e->wait;
    case GameControl::kFavorites: return &e->favorites;
    case GameControl::kTweenMenu: return &e->tweenMenu;
    default: return nullptr;
    }
}
}  // namespace

GameControl Of(const RE::BSFixedString& userEvent) {
    if (userEvent.empty()) return GameControl::kNone;
    // Pooled strings: each comparison is a pointer comparison, no string work in the input path.
    for (int i = 1; i < kGameControlCount; ++i) {
        const auto control = static_cast<GameControl>(i);
        if (const auto* name = UserEvent(control); name && *name == userEvent) return control;
    }
    return GameControl::kNone;
}

Bound BoundKeys(GameControl control) {
    Bound bound;
    auto* map = RE::ControlMap::GetSingleton();
    const auto* name = UserEvent(control);
    if (!map || !name) return bound;
    const auto kb = map->GetMappedKey(*name, RE::INPUT_DEVICE::kKeyboard);
    const auto mouse = map->GetMappedKey(*name, RE::INPUT_DEVICE::kMouse);
    const auto pad = map->GetMappedKey(*name, RE::INPUT_DEVICE::kGamepad);
    if (kb != 0xFF && kb > 0 && kb < 256) bound.key = kb;
    if (mouse != 0xFF && mouse < 8) bound.mouse = 256 + mouse;
    if (pad != 0xFF && pad > 0 && pad <= 65535) bound.gamepad = pad;
    return bound;
}

}  // namespace AnimationInjector::GameControls
