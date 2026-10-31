#include "runtime/InputSink.h"
#include "runtime/ActionRegistry.h"
#include "runtime/ActionLinks.h"
#include "runtime/Carry.h"
#include "core/GameInputReplay.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
using namespace AnimationInjector;
int main() {
    RE::UI ui;
    RE::UI::instance = &ui;
    auto* sink = InputSink::Get();
    auto send = [&](RE::InputEvent& event) {
        RE::InputEvent* first = &event;
        sink->ProcessEvent(&first, nullptr);
    };
    auto button = [&](std::string name, bool pressed, std::uint32_t code = 17) {
        RE::ButtonEvent e;
        e.userEvent = name;
        e.pressed = pressed;
        e.down = pressed;
        e.code = code;
        send(e);
    };
    for (int gate = 0; gate < 3; ++gate) {
        sink->Reset();
        Actions::runner = {};
        ActionLinks::calls = 0;
        ui = {};
        RE::UI::instance = &ui;
        button("Forward", true);
        assert(sink->AnyMoveInput() && Actions::runner.moves == 1);
        if (gate == 0)
            ui.paused = true;
        else if (gate == 1)
            ui.console = true;
        else
            RE::UI::instance = nullptr;
        const int links = ActionLinks::calls;
        button("Forward", false);
        assert(!sink->AnyMoveInput());
        button("", true, 34);
        assert(Actions::runner.requests == 0 && ActionLinks::calls == links);
        Actions::runner.state = ActionRunner::State::kActive;
        button("Forward", true);
        button("", true, 34);
        assert(sink->AnyMoveInput() && Actions::runner.moves == 1 && Actions::runner.windows == 0);
        ui = {};
        RE::UI::instance = &ui;
        assert(sink->CurrentMoveDir(MoveDir::kNone) == MoveDir::kForward);
        button("Forward", false);
        assert(!sink->AnyMoveInput());
        Actions::runner.state = ActionRunner::State::kIdle;
        button("", true, 34);
        assert(Actions::runner.requests == 1);
    }
    sink->Reset();
    Actions::runner = {};
    ui = {};
    RE::ThumbstickEvent stick;
    stick.yValue = 1;
    send(stick);
    assert(sink->AnyMoveInput() && Actions::runner.moves == 1);
    ui.paused = true;
    stick.yValue = 0;
    send(stick);
    assert(!sink->AnyMoveInput());
    stick.xValue = 1;
    send(stick);
    assert(sink->AnyMoveInput() && Actions::runner.moves == 1);
    RE::DeviceConnectEvent disconnect;
    disconnect.device.value = RE::INPUT_DEVICE::kGamepad;
    send(disconnect);
    assert(!sink->AnyMoveInput());
    button("Forward", true);
    send(disconnect);
    assert(sink->AnyMoveInput());  // Disconnecting a pad preserves keyboard input.
    disconnect.device.value = RE::INPUT_DEVICE::kKeyboard;
    send(disconnect);
    assert(!sink->AnyMoveInput());
    button("Left", true);
    sink->Reset();
    assert(!sink->AnyMoveInput());
    // Press kinds (10-30, runtime/PressKinds.h) on a key: tap at the release, hold at the hold time, double at the
    // second press; a menu drops a wait.
    ui = {};
    sink->Reset();
    sink->SetPressTiming({0.2f, 0.25f});
    Actions::runner = {};
    Actions::runner.spec.trigger.press = PressKinds::Press::kTap;
    button("", true, 34);
    assert(Actions::runner.requests == 0);
    sink->Tick(0.1f);
    button("", false, 34);
    assert(Actions::runner.requests == 1);
    button("", true, 34);
    sink->Tick(0.3f);
    button("", false, 34);
    assert(Actions::runner.requests == 1);  // too long for a tap
    Actions::runner = {};
    Actions::runner.spec.trigger.press = PressKinds::Press::kHold;
    button("", true, 34);
    sink->Tick(0.19f);
    assert(Actions::runner.requests == 0);
    sink->Tick(0.02f);
    assert(Actions::runner.requests == 1);  // at the hold time, still down
    button("", false, 34);
    assert(Actions::runner.requests == 1);
    Actions::runner = {};
    Actions::runner.spec.trigger.press = PressKinds::Press::kDouble;
    button("", true, 34);
    button("", false, 34);
    sink->Tick(0.1f);
    button("", true, 34);
    assert(Actions::runner.requests == 1);  // at the second press
    sink->Tick(1.0f);
    button("", false, 34);
    assert(Actions::runner.requests == 1);  // a held second press is still the double
    Actions::runner = {};
    Actions::runner.spec.trigger.press = PressKinds::Press::kTap;
    button("", true, 34);
    ui.console = true;
    button("", false, 34);
    ui = {};
    sink->Tick(1.0f);
    assert(Actions::runner.requests == 0);
    // Nothing could start when it went down: no wait, nothing later.
    Actions::startable = false;
    button("", true, 34);
    Actions::startable = true;
    button("", false, 34);
    assert(Actions::runner.requests == 0);
    ui = {};
    sink->Reset();
    Actions::runner = {};
    RE::ButtonEvent trigger, move;
    trigger.code = 34;
    trigger.pressed = trigger.down = true;
    trigger.next = &move;
    move.code = 17;
    move.pressed = move.down = true;
    move.userEvent = "Back";
    send(trigger);
    // The key starts its own action; a movement key in the same batch only feeds the direction condition.
    assert(Actions::runner.requests == 1 && Actions::runner.lastOrigin == 0);
    // A start on a game control (0.12.292): any device's press of it starts the action, and that press and its
    // release are kept from the game; a press that starts nothing goes to the game.
    sink->Reset();
    Actions::runner = {};
    Actions::runner.spec.trigger.key = 0;
    Actions::runner.spec.trigger.gamepad = 0;
    Actions::runner.spec.trigger.control = GameControl::kJump;
    RE::ButtonEvent jump;
    jump.userEvent = "Jump";
    jump.code = 57;
    jump.pressed = jump.down = true;
    send(jump);
    assert(Actions::runner.requests == 1 && sink->Taken(&jump));
    jump.pressed = jump.down = false;
    send(jump);
    assert(sink->Taken(&jump));  // its release too
    RE::ButtonEvent padJump = jump;
    padJump.code = 32768;
    padJump.device.value = RE::INPUT_DEVICE::kGamepad;
    padJump.pressed = padJump.down = true;
    send(padJump);
    assert(Actions::runner.requests == 2 && sink->Taken(&padJump));
    padJump.pressed = padJump.down = false;
    send(padJump);
    Actions::runner.accept = false;
    jump.pressed = jump.down = true;
    send(jump);
    assert(Actions::runner.requests == 3 && !sink->Taken(&jump));
    jump.pressed = jump.down = false;
    send(jump);
    assert(!sink->Taken(&jump));
    RE::ButtonEvent sprint;
    sprint.userEvent = "Sprint";
    sprint.code = 57;
    sprint.pressed = sprint.down = true;
    send(sprint);
    assert(Actions::runner.requests == 3 && !sink->Taken(&sprint));  // the same key under another control
    // Drawing or sheathing (0.12.298): kept from the game while the player's action plays, press and release; the
    // game gets it when nothing plays.
    sink->Reset();
    Actions::runner = {};
    RE::ButtonEvent ready;
    ready.userEvent = "Ready Weapon";
    ready.code = 19;
    ready.pressed = ready.down = true;
    send(ready);
    assert(!sink->Taken(&ready));
    ready.pressed = ready.down = false;
    send(ready);
    assert(!sink->Taken(&ready));
    Actions::runner.state = ActionRunner::State::kActive;
    ready.pressed = ready.down = true;
    send(ready);
    assert(sink->Taken(&ready) && Actions::runner.requests == 0);
    Actions::runner.state = ActionRunner::State::kIdle;
    ready.pressed = ready.down = false;
    send(ready);
    assert(sink->Taken(&ready));  // the release of a kept press, after the action ended
    // A game control's tap (10-30): the press waits, kept from the game. A tap starts the action; a long press is
    // the game's, handed to it at the hold time with its release going through; a tap whose action then says no is
    // handed to the game whole.
    sink->Reset();
    sink->SetPressTiming({0.2f, 0.25f});
    Actions::runner = {};
    Actions::runner.spec.trigger.key = 0;
    Actions::runner.spec.trigger.gamepad = 0;
    Actions::runner.spec.trigger.control = GameControl::kJump;
    Actions::runner.spec.trigger.press = PressKinds::Press::kTap;
    GameInputReplay::sent.clear();
    jump.pressed = jump.down = true;
    send(jump);
    assert(sink->Taken(&jump) && Actions::runner.requests == 0);
    sink->Tick(0.1f);
    jump.pressed = jump.down = false;
    send(jump);
    assert(sink->Taken(&jump) && Actions::runner.requests == 1 && GameInputReplay::sent.empty());
    jump.pressed = jump.down = true;
    send(jump);
    assert(sink->Taken(&jump));
    jump.down = false;  // a held repeat while it waits: kept from the game too
    send(jump);
    assert(sink->Taken(&jump));
    sink->Tick(0.25f);  // the hold time passes: the game's press, sent on the next tick
    sink->Tick(0.01f);
    assert(GameInputReplay::sent.size() == 1 && GameInputReplay::sent[0].userEvent == "Jump" &&
           !GameInputReplay::sent[0].tap);
    send(jump);
    assert(!sink->Taken(&jump));  // held repeats are the game's once it has the press
    jump.pressed = jump.down = false;
    send(jump);
    assert(!sink->Taken(&jump) && Actions::runner.requests == 1);  // its release goes to the game
    Actions::runner.accept = false;
    jump.pressed = jump.down = true;
    send(jump);
    sink->Tick(0.1f);
    jump.pressed = jump.down = false;
    send(jump);
    assert(sink->Taken(&jump) && Actions::runner.requests == 2);  // the tap was tried and said no
    sink->Tick(0.01f);
    assert(GameInputReplay::sent.size() == 2 && GameInputReplay::sent[1].tap);  // the game gets it whole
    // A jump press when nothing could start goes to the game at once, without a wait.
    Actions::startable = false;
    jump.pressed = jump.down = true;
    send(jump);
    assert(!sink->Taken(&jump));
    jump.pressed = jump.down = false;
    send(jump);
    assert(!sink->Taken(&jump));
    Actions::startable = true;
    sink->Tick(1.0f);
    assert(GameInputReplay::sent.size() == 2 && Actions::runner.requests == 2);
    std::cout << "InputSink menu, console, missing UI, held input, device disconnect, batch origin, reset, game "
                 "control, ready weapon and press kind checks passed\n";
}
