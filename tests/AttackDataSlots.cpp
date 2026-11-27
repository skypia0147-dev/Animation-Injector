#include "runtime/AttackDataSlots.h"
#include <cstdlib>
#include <iostream>

using Slots = AnimationInjector::AttackDataSlots<int>;
int main() {
    unsigned checks = 0;
    auto check = [&](bool condition, const char* what) {
        ++checks;
        if (!condition) {
            std::cerr << "Failed check " << checks << ": " << what << '\n';
            std::exit(1);
        }
    };
    // Attack data are numbers here; 0 is none. `engine` is the engine's value, written as the slots say.
    int engine = 0;
    auto write = [&](std::optional<int> value) {
        if (value) engine = *value;
        return value.has_value();
    };
    {
        // An action alone: its data while it plays, the value from before when it ends.
        Slots s;
        engine = 7;
        check(write(s.SetAction(engine, 1, 10)) && engine == 10, "the action's data");
        check(write(s.ClearAction(engine, 1)) && engine == 7 && s.Empty(), "the value from before comes back");
    }
    {
        // An event over an action: the event's while it is on, the action's again at its off.
        Slots s;
        engine = 7;
        write(s.SetAction(engine, 1, 10));
        check(write(s.SetEvent(engine, 20)) && engine == 20 && s.EventInEffect(engine),
            "the event's is over the action's");
        check(write(s.SetEvent(engine, 21)) && engine == 21, "a later on replaces the earlier one");
        check(
            write(s.ClearEvent(engine)) && engine == 10 && !s.EventInEffect(engine), "its off gives the action's back");
        check(write(s.ClearAction(engine, 1)) && engine == 7 && s.Empty(), "then the value from before");
    }
    {
        // The action ends while the event is still on: the event's stays, and its end gives the value from before.
        Slots s;
        engine = 7;
        write(s.SetAction(engine, 1, 10));
        write(s.SetEvent(engine, 20));
        check(write(s.ClearAction(engine, 1)) && engine == 20 && !s.Empty(), "the event outlives the action");
        check(write(s.ClearEvent(engine)) && engine == 7 && s.Empty(), "never the ended action's data");
    }
    {
        // An event with no action (another mod's clip): on and off around the engine's own value.
        Slots s;
        engine = 7;
        check(write(s.SetEvent(engine, 20)) && engine == 20, "an event alone");
        check(write(s.ClearEvent(engine)) && engine == 7 && s.Empty(), "back to the engine's");
        check(!write(s.ClearEvent(engine)) && engine == 7, "an off with nothing on writes nothing");
    }
    {
        // A chain: the next action sets its data before the one before it ends.
        Slots s;
        engine = 7;
        write(s.SetAction(engine, 1, 10));
        check(write(s.SetAction(engine, 2, 11)) && engine == 11, "the next action's data");
        check(!write(s.ClearAction(engine, 1)) && engine == 11, "the earlier action's end leaves it");
        check(
            write(s.ClearAction(engine, 2)) && engine == 7 && s.Empty(), "the chain's end gives the first value back");
    }
    {
        // The engine replaced the value (a vanilla attack started): it is the engine's, nothing is written back.
        Slots s;
        engine = 7;
        write(s.SetAction(engine, 1, 10));
        write(s.SetEvent(engine, 20));
        engine = 30;
        check(!s.EventInEffect(engine), "a replaced event is not in effect");
        check(!write(s.ClearEvent(engine)) && engine == 30 && s.Empty(), "the engine's value stays");
        check(!write(s.ClearAction(engine, 1)) && engine == 30, "and the action's end leaves it too");
        // What comes after starts from the engine's value.
        check(write(s.SetEvent(engine, 21)) && engine == 21, "a new on after it");
        check(write(s.ClearEvent(engine)) && engine == 30, "gives the engine's value back");
    }
    {
        // Replaced, then the same action sets again (a restart): from the engine's new value.
        Slots s;
        engine = 7;
        write(s.SetAction(engine, 1, 10));
        engine = 30;
        check(write(s.SetAction(engine, 1, 10)) && engine == 10, "set again after a replacement");
        check(write(s.ClearAction(engine, 1)) && engine == 30, "the replacement is what comes back");
    }
    std::cout << checks << " attack data slot checks passed\n";
    return 0;
}
