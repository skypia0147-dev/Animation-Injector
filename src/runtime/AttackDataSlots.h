#pragma once
#include <optional>

// Who decides an actor's attack data while we change it (1.1.0), without engine headers (tests).
//
// Two of ours may name it at once: the action playing on the actor (<Action attack>, its runner) and an event
// function (attackDataOn, runtime/EventEffects.h). The event's is over the action's, and when both are gone the value
// from before the first of them comes back. Each change is told the engine's value now: one that is no longer what we
// wrote last was replaced by the engine (a vanilla attack started), which owns it from then on, so everything of ours
// is dropped and nothing is written back.
//
// T is a handle to attack data: default-constructed is none, comparable, false when none.
namespace AnimationInjector {
template<class T>
struct AttackDataSlots {
    T original{}, action{}, event{}, written{};
    int owner = -1;        // the action slot's owner (its action index)
    bool holding = false;  // `written` is in the engine, `original` is what was there before

    // What to write into the engine, if anything, for each change. `current` is the engine's value now.
    std::optional<T> SetAction(const T& current, int by, const T& data) {
        Begin(current);
        action = data;
        owner = by;
        return Write();
    }
    // Only the owner's: an action that ended after the next one of a chain set its own leaves that one alone.
    std::optional<T> ClearAction(const T& current, int by) {
        if (!holding || owner != by || Replaced(current)) return std::nullopt;
        action = T{};
        owner = -1;
        return Write();
    }
    std::optional<T> SetEvent(const T& current, const T& data) {
        Begin(current);
        event = data;
        return Write();
    }
    std::optional<T> ClearEvent(const T& current) {
        if (!holding || !event || Replaced(current)) return std::nullopt;
        event = T{};
        return Write();
    }
    // The event's data is what the engine has now.
    bool EventInEffect(const T& current) const { return holding && event && current == written; }
    // Nothing of ours is left: the entry can go.
    bool Empty() const { return !holding; }

private:
    // The engine changed the value since we wrote it: it is the engine's now.
    bool Replaced(const T& current) {
        if (current == written) return false;
        *this = AttackDataSlots{};
        return true;
    }
    void Begin(const T& current) {
        if (holding) Replaced(current);
        if (!holding) {
            original = current;
            holding = true;
        }
    }
    std::optional<T> Write() {
        const bool none = !action && !event;
        const T value = event ? event : action ? action : original;
        if (none) {
            *this = AttackDataSlots{};
            return value;
        }
        written = value;
        return value;
    }
};
}
