#include "PCH.h"
#include "core/GameInputReplay.h"

namespace AnimationInjector::GameInputReplay {
namespace {
bool g_replaying = false;

void Dispatch(RE::BSInputDeviceManager* manager, RE::INPUT_DEVICE device, std::uint32_t idCode,
    const RE::BSFixedString& userEvent, float value, float held) {
    auto* event = RE::ButtonEvent::Create(device, userEvent, idCode, value, held);
    if (!event) return;
    RE::InputEvent* head = event;
    manager->SendEvent(&head);
    RE::free(event);
}
}

void Send(RE::INPUT_DEVICE device, std::uint32_t idCode, const RE::BSFixedString& userEvent, bool tap) {
    auto* manager = RE::BSInputDeviceManager::GetSingleton();
    if (!manager || g_replaying) return;
    g_replaying = true;
    struct Scope {
        ~Scope() { g_replaying = false; }
    } scope;
    Dispatch(manager, device, idCode, userEvent, 1.0f, 0.0f);
    // A whole press: its release with it, so the game is never left holding a key that is up.
    if (tap) Dispatch(manager, device, idCode, userEvent, 0.0f, 0.1f);
    logger::debug("[Press] late game press '{}' {}", userEvent.c_str(), tap ? "tap" : "down");
}

bool Replaying() { return g_replaying; }
}  // namespace AnimationInjector::GameInputReplay
