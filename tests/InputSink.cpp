#include "runtime/InputSink.h"
#include "runtime/ActionRegistry.h"
#include "runtime/ActionLinks.h"
#include "runtime/Carry.h"
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
    ui = {};
    Actions::runner = {};
    Actions::runner.spec.trigger.onKeyUp = true;
    button("", true, 34);
    assert(Actions::runner.requests == 0);
    button("", false, 34);
    assert(Actions::runner.requests == 1);
    ui.console = true;
    button("", false, 34);
    assert(Actions::runner.requests == 1);
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
    std::cout
        << "InputSink menu, console, missing UI, held input, device disconnect, batch origin and reset checks passed\n";
}
