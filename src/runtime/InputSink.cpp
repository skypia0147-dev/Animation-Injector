#include "PCH.h"
#include "runtime/InputSink.h"
#include "runtime/ActionRegistry.h"
#include "runtime/ActionRunner.h"
#include "runtime/ActionLinks.h"
#include "runtime/Carry.h"
#include "runtime/QteServices.h"
#include "runtime/EventSuppression.h"
#include "runtime/GameControlBindings.h"
#include "core/InputDispatchHook.h"
#include <climits>

namespace AnimationInjector {
namespace {
// Move input reaches only the runners the player has used; a runner that never ran is idle and
// ignores it (ActionRunner::OnMoveInput needs an open recovery).
void MoveInputToRunners(int kind) {
    Actions::FindRunner(RE::PlayerCharacter::GetSingleton(), [&](int, ActionRunner& runner) {
        runner.OnMoveInput(kind);
        return false;
    });
}
}

InputSink* InputSink::Get() {
    static InputSink s;
    return &s;
}

void InputSink::Register() {
    if (InputDispatchHook::InstallIfNeeded()) return;
    if (auto* dm = RE::BSInputDeviceManager::GetSingleton()) {
        dm->AddEventSink(Get());
        logger::info("[Input] sink registered");
    }
}

bool InputSink::Taken(const RE::InputEvent* event) const {
    return std::find(_taken.begin(), _taken.end(), event) != _taken.end();
}

bool InputSink::AnyMoveInput() const {
    return _fwd || _back || _left || _right || std::fabs(_stickX) >= 0.25f || std::fabs(_stickY) >= 0.25f;
}

MoveDir InputSink::CurrentMoveDir(MoveDir a_default) const {
    float x = 0.0f, y = 0.0f;
    if (_fwd) y += 1.0f;
    if (_back) y -= 1.0f;
    if (_right) x += 1.0f;
    if (_left) x -= 1.0f;
    if (x == 0.0f && y == 0.0f) {
        x = _stickX;
        y = _stickY;
    }
    if (_batchMove) {
        x = _batchX;
        y = _batchY;
    }
    if (std::fabs(x) < 0.25f && std::fabs(y) < 0.25f) return a_default;
    if (std::fabs(y) >= std::fabs(x)) return y > 0 ? MoveDir::kForward : MoveDir::kBack;
    return x > 0 ? MoveDir::kRight : MoveDir::kLeft;
}

RE::BSEventNotifyControl InputSink::ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) {
    _taken.clear();
    if (!a_event || !*a_event) return RE::BSEventNotifyControl::kContinue;
    // An active carry's putdown key never reaches this sink: the input dispatch hook claims it
    // (Carry::ClaimInput) before PlayerControls, so the press that starts a carry is not also read
    // as its cancellation.
    auto* ui = RE::UI::GetSingleton();
    const bool allowActions = ui && !ui->GameIsPaused() && !ui->IsMenuOpen(RE::Console::MENU_NAME);
    auto* ue = RE::UserEvents::GetSingleton();

    // Direction and strict eligibility see the entire current batch, even when
    // the mouse button precedes a movement change in the linked event list.
    bool fwd = _fwd, back = _back, left = _left, right = _right;
    float sx = _stickX, sy = _stickY;
    for (auto* ev = *a_event; ev; ev = ev->next) {
        QteServices::NoteInput(ev);  // A QTE key's name follows the device in use.
        if (ev->eventType == RE::INPUT_EVENT_TYPE::kDeviceConnect &&
            !static_cast<RE::DeviceConnectEvent*>(ev)->connected) {
            if (ev->device.get() == RE::INPUT_DEVICE::kKeyboard)
                fwd = back = left = right = false;
            else if (ev->device.get() == RE::INPUT_DEVICE::kGamepad)
                sx = sy = 0;
        } else if (ev->eventType == RE::INPUT_EVENT_TYPE::kThumbstick) {
            auto* stick = static_cast<RE::ThumbstickEvent*>(ev);
            if (stick->IsLeft()) {
                sx = stick->xValue;
                sy = stick->yValue;
            }
        } else if (ue && ev->eventType == RE::INPUT_EVENT_TYPE::kButton) {
            auto* button = ev->AsButtonEvent();
            if (!button) continue;
            const auto& name = button->QUserEvent();
            const bool down = button->IsPressed();
            if (name == ue->forward)
                fwd = down;
            else if (name == ue->back)
                back = down;
            else if (name == ue->strafeLeft)
                left = down;
            else if (name == ue->strafeRight)
                right = down;
        }
    }
    _batchX = static_cast<float>(right) - static_cast<float>(left);
    _batchY = static_cast<float>(fwd) - static_cast<float>(back);
    if (_batchX == 0 && _batchY == 0) {
        _batchX = sx;
        _batchY = sy;
    }
    _batchMove = true;
    struct BatchScope {
        bool& active;
        ~BatchScope() { active = false; }
    } batchScope{_batchMove};
    for (auto* ev = *a_event; ev; ev = ev->next) {
        // Device state must be retired even while gameplay input is blocked.
        if (ev->eventType == RE::INPUT_EVENT_TYPE::kDeviceConnect) {
            if (!static_cast<RE::DeviceConnectEvent*>(ev)->connected) {
                if (ev->device.get() == RE::INPUT_DEVICE::kGamepad)
                    _stickX = _stickY = 0.0f;
                else if (ev->device.get() == RE::INPUT_DEVICE::kKeyboard)
                    _fwd = _back = _left = _right = false;
            }
            continue;
        }
        if (ev->eventType == RE::INPUT_EVENT_TYPE::kThumbstick) {
            auto* te = static_cast<RE::ThumbstickEvent*>(ev);
            if (te && te->IsLeft()) {
                const bool was = std::fabs(_stickX) >= 0.25f || std::fabs(_stickY) >= 0.25f;
                _stickX = te->xValue;
                _stickY = te->yValue;
                const bool now = std::fabs(_stickX) >= 0.25f || std::fabs(_stickY) >= 0.25f;
                if (allowActions && now && !was) MoveInputToRunners(0);
            }
            continue;
        }
        if (ev->eventType != RE::INPUT_EVENT_TYPE::kButton) continue;
        auto* be = ev->AsButtonEvent();
        if (!be) continue;

        const auto dev = be->device.get();
        const auto id = be->GetIDCode();
        const bool gamepad = dev == RE::INPUT_DEVICE::kGamepad;
        const std::uint32_t code = dev == RE::INPUT_DEVICE::kMouse ? id + 256 : id;
        // The game control this press stands for (0.12.292): a start or transition on it matches any device.
        const auto control = GameControls::Of(be->QUserEvent());
        auto& swallow = _swallowRelease[static_cast<int>(control)];
        const auto keep = [&] {
            if (!Taken(ev)) _taken.push_back(ev);
            if (be->IsDown()) swallow = true;
        };
        // The release of a press kept from the game is kept too: the game never saw it go down.
        if (control != GameControl::kNone && be->IsUp() && std::exchange(swallow, false)) _taken.push_back(ev);
        // Snapshot ownership before recovery or graph callbacks can retire a runner.
        const auto link =
            allowActions ? ActionLinks::Input(code, gamepad, control, be->IsDown()) : ActionLinks::InputResult{};
        const bool comboInput = link.claimed;
        if (link.taken && control != GameControl::kNone) keep();
        // Drawing or sheathing is ignored while the player's action plays (user decision 10-26): the game never sees
        // the press, so nothing waits to happen when the action ends. A request from elsewhere (a script, an NPC's
        // AI) still reaches the graph and plays when the action exits (ActionRunner ReplayWeaponRequest).
        if (allowActions && control == GameControl::kReadyWeapon && !link.taken &&
            Actions::FindRunner(RE::PlayerCharacter::GetSingleton(),
                [](int, ActionRunner& r) { return r.GetState() == ActionRunner::State::kActive; }))
            keep();

        if (ue) {  // Track releases in menus without dispatching recovery or action input.
            const auto& uev = be->QUserEvent();
            const bool pressed = be->IsPressed();
            int kind = -1;
            if (uev == ue->forward) {
                _fwd = pressed;
                kind = 0;
            } else if (uev == ue->back) {
                _back = pressed;
                kind = 0;
            } else if (uev == ue->strafeLeft) {
                _left = pressed;
                kind = 0;
            } else if (uev == ue->strafeRight) {
                _right = pressed;
                kind = 0;
            } else if (uev == ue->sprint)
                kind = 1;
            else if (uev == ue->jump)
                kind = 2;
            if (allowActions && kind >= 0 && be->IsDown()) MoveInputToRunners(kind);  // Recovery
        }

        if (!allowActions) continue;

        // The actions this button starts. A running one ignores it: what follows it takes its keys through
        // ActionLinks (comboInput above). There are no combo windows any more (format 4).
        static const std::vector<int> none;
        const auto& keyed = dev == RE::INPUT_DEVICE::kGamepad ? Actions::ActionsForGamepad(id)
                            : dev == RE::INPUT_DEVICE::kKeyboard || dev == RE::INPUT_DEVICE::kMouse
                                ? Actions::ActionsForKey(code)
                                : none;
        auto* owner = RE::PlayerCharacter::GetSingleton();
        const auto& controlled = Actions::ActionsForControl(control);
        for (const auto* list : {&keyed, &controlled})
            for (const int a : *list) {
                if (comboInput) break;
                if (auto* running = Actions::RunnerFor(owner, a, false);
                    running && running->GetState() == ActionRunner::State::kActive)
                    continue;
                const auto& spec = Actions::Spec(a);
                const auto& t = spec.trigger;
                const bool fire = t.onKeyUp ? be->IsUp() : be->IsDown();
                if (fire) {
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    const auto partner = Actions::CaptureTarget(player, a);
                    const int selected = spec.paired.enabled ? Actions::SelectPairedStart(player, a, partner)
                                                             : Actions::SelectStart(player, a, partner);
                    if (selected < 0) {
                        EventSuppression::RecordRequest(a, -1, false);
                        continue;
                    }
                    auto& destination = Actions::Runner(selected);
                    bool started = false;
                    if (destination.Spec().paired.enabled) {
                        if (destination.ValidPairedTarget(partner)) {
                            destination.RequestPaired(partner);
                            started = true;
                        }
                    } else {
                        started = destination.Request(a);
                        EventSuppression::RecordRequest(a, selected, started);
                    }
                    // A game control's press that starts one of ours is the action's alone: the game does not jump
                    // too. When nothing starts (a condition says no) the game acts on it as usual.
                    if (started && list == &controlled) keep();
                }
            }
    }
    return RE::BSEventNotifyControl::kContinue;
}

}  // namespace AnimationInjector
