#include "PCH.h"
#include "runtime/QteServices.h"
#include "runtime/GameControlBindings.h"
#include "runtime/ActionLinks.h"
#include "runtime/ActionRegistry.h"
#include "runtime/GetUpHandoff.h"
#include <atomic>
#include <charconv>
#include <cmath>
namespace AnimationInjector::QteServices {
bool ResolveControl(QteStyle& style, GameControl control) {
    const auto bound = GameControls::BoundKeys(control);
    style.key = bound.key ? bound.key : bound.mouse;
    style.alternateKey = bound.key ? bound.mouse : 0;
    style.gamepad = bound.gamepad;
    // Nothing bound (or no control map yet): the attack button, as before for rightAttack.
    if (!style.key && !style.gamepad) style.key = 256;
    return true;
}
namespace {
// -1 until the player presses something; 1 = gamepad, 0 = keyboard or mouse.
std::atomic<int> lastDevice{-1};
}
void NoteInput(const RE::InputEvent* event) {
    if (!event) return;
    const auto device = event->device.get();
    if (event->eventType == RE::INPUT_EVENT_TYPE::kDeviceConnect) {
        if (device == RE::INPUT_DEVICE::kGamepad && !static_cast<const RE::DeviceConnectEvent*>(event)->connected)
            lastDevice = 0;
        return;
    }
    if (event->eventType == RE::INPUT_EVENT_TYPE::kThumbstick) {
        const auto* stick = static_cast<const RE::ThumbstickEvent*>(event);
        if (std::fabs(stick->xValue) >= .5f || std::fabs(stick->yValue) >= .5f) lastDevice = 1;
        return;
    }
    if (event->eventType != RE::INPUT_EVENT_TYPE::kButton) return;
    const auto* button = static_cast<const RE::ButtonEvent*>(event);
    if (!button->IsDown()) return;
    if (device == RE::INPUT_DEVICE::kGamepad)
        lastDevice = 1;
    else if (device == RE::INPUT_DEVICE::kKeyboard || device == RE::INPUT_DEVICE::kMouse)
        lastDevice = 0;
}
bool UsingGamepad() {
    const int last = lastDevice.load();
    if (last >= 0) return last == 1;
    auto* devices = RE::BSInputDeviceManager::GetSingleton();
    return devices && devices->IsGamepadEnabled();
}
std::uint32_t ResolveSound(const QteResultSpec& result) {
    if (result.soundPlugin.empty() || result.soundForm.empty()) return 0;
    auto* data = RE::TESDataHandler::GetSingleton();
    if (!data) return 0;
    std::string_view digits = result.soundForm;
    if (digits.starts_with("0x") || digits.starts_with("0X")) digits.remove_prefix(2);
    std::uint32_t id = 0;
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), id, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) return 0;
    auto* sound = data->LookupForm<RE::BGSSoundDescriptorForm>(id, result.soundPlugin);
    if (!sound) {
        logger::warn("[QTE] sound unavailable: {}:{}", result.soundPlugin, result.soundForm);
        return 0;
    }
    return sound->GetFormID();
}
void StartAction(RE::ActorHandle actor, const std::string& action, const std::string& partnerAction) {
    const int index = action.empty() ? -1 : Actions::IndexOf(action);
    const int partnerIndex = partnerAction.empty() ? -1 : Actions::IndexOf(partnerAction);
    if (!partnerAction.empty() && partnerIndex < 0)
        logger::warn("[QTE] result partner action '{}' unavailable", partnerAction);
    // In a pair the result hands both over the way a Connect with partnerTo does.
    if (ActionLinks::RequestHandoff(actor, index, partnerIndex)) {
        logger::info("[QTE] result in a pair: player -> '{}', partner -> '{}'", action, partnerAction);
        return;
    }
    auto target = actor.get();
    if (index < 0 || !target) {
        logger::warn("[QTE] result action '{}' unavailable", action);
        return;
    }
    GetUpHandoff::Start(target.get(), index);
}
void PlaySound(std::uint32_t form) {
    if (!form) return;
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || !player->Is3DLoaded()) return;
    auto* sound = RE::TESForm::LookupByID<RE::BGSSoundDescriptorForm>(form);
    auto* manager = RE::BSAudioManager::GetSingleton();
    if (!sound || !manager) return;
    RE::BSSoundHandle handle;
    if (manager->GetSoundHandle(handle, sound)) {
        handle.SetObjectToFollow(player->Get3D());
        handle.Play();
    }
}
}
