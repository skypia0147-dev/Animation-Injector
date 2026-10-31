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
#include "core/GameInputReplay.h"
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

bool InputSink::MoveVector(float& x, float& y) const {
    x = y = 0.0f;
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
    return std::fabs(x) >= 0.25f || std::fabs(y) >= 0.25f;
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
    if (!a_event || !*a_event || GameInputReplay::Replaying()) return RE::BSEventNotifyControl::kContinue;
    // An active carry's putdown key never reaches this sink: the input dispatch hook claims it
    // (Carry::ClaimInput) before PlayerControls, so the press that starts a carry is not also read
    // as its cancellation.
    auto* ui = RE::UI::GetSingleton();
    const bool allowActions = ui && !ui->GameIsPaused() && !ui->IsMenuOpen(RE::Console::MENU_NAME);
    auto* ue = RE::UserEvents::GetSingleton();
    // A menu or the console drops what waits to tell a press's kind (runtime/PressKinds.h).
    if (!allowActions) DropWaits();

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
        // What the player holds (runtime/HeldKeys.h, charge), seen in menus too so a release there is not missed.
        const HeldKeys::Key heldKey{gamepad ? HeldKeys::Source::kPad : HeldKeys::Source::kKey, gamepad ? id : code};
        const HeldKeys::Key heldControl{HeldKeys::Source::kControl, static_cast<std::uint32_t>(control)};
        if (be->IsDown() || be->IsUp()) {
            auto& held = HeldKeys::Get();
            const bool down = be->IsDown();
            const auto note = [&](HeldKeys::Key k) { down ? held.Press(k, _now) : held.Release(k, _now); };
            note(heldKey);
            if (control != GameControl::kNone) note(heldControl);
        }
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
        // A key transition took the press: its key is now the one an on="release" transition waits for.
        if (link.taken && be->IsDown())
            HeldKeys::Get().SetChain(control != GameControl::kNone ? heldControl : heldKey, _now);
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

        // The actions this button starts, by how it is pressed (runtime/PressKinds.h). A running one ignores it:
        // what follows it takes its keys through ActionLinks (comboInput above).
        if (comboInput) continue;
        const bool pad = dev == RE::INPUT_DEVICE::kGamepad;
        if (pad || dev == RE::INPUT_DEVICE::kKeyboard || dev == RE::INPUT_DEVICE::kMouse)
            OnPress(*be, pad ? Source::kPad : Source::kKey, pad ? id : code);
        // A game control's press that starts one of ours, or that waits to tell its kind, is kept from the game:
        // the game does not jump too. A press nothing takes goes to the game as usual.
        if (control != GameControl::kNone && OnPress(*be, Source::kControl, static_cast<std::uint32_t>(control)))
            keep();
    }
    return RE::BSEventNotifyControl::kContinue;
}

const std::vector<int>& InputSink::ActionsOf(Source source, std::uint32_t code) {
    return source == Source::kPad   ? Actions::ActionsForGamepad(code)
           : source == Source::kKey ? Actions::ActionsForKey(code)
                                    : Actions::ActionsForControl(static_cast<GameControl>(code));
}

bool InputSink::Start(
    const std::vector<int>& list, PressKinds::Press press, ConditionValues::Request* request, bool dry) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    bool started = false;
    for (const int a : list) {
        const auto& spec = Actions::Spec(a);
        if (spec.trigger.press != press) continue;
        if (auto* running = Actions::RunnerFor(player, a, false);
            running && running->GetState() == ActionRunner::State::kActive)
            continue;
        // A check reads the conditions on a copy: the press keeps its own snapshot for the start that follows.
        ConditionValues::Request copy;
        if (request) copy = *request;
        auto* conditions = dry ? &copy : request;
        const auto partner = Actions::CaptureTarget(player, a);
        const int selected = spec.paired.enabled ? Actions::SelectPairedStart(player, a, partner, conditions)
                                                 : Actions::SelectStart(player, a, partner, conditions);
        if (selected < 0) {
            if (!dry) EventSuppression::RecordRequest(a, -1, false);
            continue;
        }
        auto& destination = Actions::Runner(selected);
        if (destination.Spec().paired.enabled) {
            if (!destination.ValidPairedTarget(partner)) continue;
            if (dry) return true;
            destination.RequestPaired(partner);
            started = true;
        } else {
            if (dry) return true;
            const bool accepted = destination.Request(a);
            EventSuppression::RecordRequest(a, selected, accepted);
            started |= accepted;
        }
    }
    return started;
}

InputSink::Pressed* InputSink::Find(Source source, std::uint32_t code) {
    for (auto& p : _pressed)
        if (p.source == source && p.code == code) return &p;
    return nullptr;
}

bool InputSink::OnPress(const RE::ButtonEvent& event, Source source, std::uint32_t code) {
    const auto& list = ActionsOf(source, code);
    auto* pressed = Find(source, code);
    if (list.empty() && !pressed) return false;
    const bool game = source == Source::kControl;
    if (pressed) Settle(*pressed);  // an expired wait first
    if (event.IsDown()) {
        // A start on the moment it goes down takes the press whole, before any wait.
        if (Start(list, PressKinds::Press::kDown, nullptr, false)) {
            HeldKeys::Get().SetChain({source, code}, _now);
            return true;
        }
        PressKinds::Kinds kinds;
        if (!pressed || !pressed->classifier.Waiting()) {
            // The first press: the kinds that could start now, with this moment's conditions (movement direction).
            ConditionValues::Request request;
            Actions::CaptureMovement(request);
            kinds.tap = Start(list, PressKinds::Press::kTap, &request, true);
            kinds.dbl = Start(list, PressKinds::Press::kDouble, &request, true);
            kinds.hold = Start(list, PressKinds::Press::kHold, &request, true);
            if (!kinds.Any()) return false;
            if (!pressed) {
                pressed = &_pressed.emplace_back();
                pressed->source = source;
                pressed->code = code;
            }
            pressed->request = request;
            pressed->downAt = _now;
            pressed->device = event.device.get();
            pressed->idCode = event.GetIDCode();
            pressed->userEvent = event.QUserEvent();
        }
        pressed->down = true;
        const auto step = pressed->classifier.Down(_now, kinds, game, _timing);
        Act(*pressed, step.outcome);
        return step.keep;
    }
    if (!pressed) return false;
    if (!event.IsUp()) return pressed->classifier.Keeps();  // a held repeat
    pressed->down = false;
    // The release of a press kept from the game is kept by the sink's own rule (_swallowRelease).
    Act(*pressed, pressed->classifier.Up(_now, _timing).outcome);
    return false;
}

void InputSink::Settle(Pressed& pressed) { Act(pressed, pressed.classifier.Tick(_now, _timing)); }

void InputSink::Act(Pressed& pressed, PressKinds::Outcome outcome) {
    using Outcome = PressKinds::Outcome;
    if (outcome == Outcome::kNone) return;
    if (outcome == Outcome::kGameDown || outcome == Outcome::kGameTap) {
        ToGame(pressed, outcome == Outcome::kGameTap);
        return;
    }
    const auto press = outcome == Outcome::kTap      ? PressKinds::Press::kTap
                       : outcome == Outcome::kDouble ? PressKinds::Press::kDouble
                                                     : PressKinds::Press::kHold;
    const bool started = Start(ActionsOf(pressed.source, pressed.code), press, &pressed.request, false);
    logger::debug("[Press] {} {}: {} started={}", pressed.source == Source::kControl ? "control" : "key", pressed.code,
        outcome == Outcome::kTap      ? "tap"
        : outcome == Outcome::kDouble ? "double"
                                      : "hold",
        started);
    // The held time of what it started counts from its first press (a hold's charge, runtime/HeldKeys.h).
    if (started) HeldKeys::Get().SetChain({pressed.source, pressed.code}, pressed.downAt);
    if (started || pressed.source != Source::kControl) return;
    // Its actions started nothing after all (a condition changed while it waited): the press is the game's.
    if (pressed.down) pressed.classifier.ToGame();
    ToGame(pressed, !pressed.down);
}

void InputSink::ToGame(Pressed& pressed, bool tap) {
    pressed.send = true;
    pressed.sendTap = tap;
    // A key still down: the game gets its press now and its release when it comes.
    if (!tap) _swallowRelease[pressed.code] = false;
}

void InputSink::Tick(float delta) {
    if (!std::isfinite(delta) || delta < 0.0f) return;
    _now += delta;
    for (auto& pressed : _pressed) Settle(pressed);
    // Late presses go out here, outside the input dispatch. A key that came up before its press went out gets a
    // whole one, so the game is never left holding it.
    for (auto& pressed : _pressed)
        if (std::exchange(pressed.send, false))
            GameInputReplay::Send(pressed.device, pressed.idCode, pressed.userEvent, pressed.sendTap || !pressed.down);
}

void InputSink::DropWaits() {
    for (auto& pressed : _pressed) {
        pressed.classifier.Reset();
        pressed.send = false;
    }
}

}  // namespace AnimationInjector
