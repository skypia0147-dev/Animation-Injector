#include "runtime/PressKinds.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>

// A start's press kinds (10-30): tap, double and hold on one key or game control.
int main() {
    using namespace AnimationInjector::PressKinds;
    const Timing timing{0.2f, 0.25f};
    const Kinds tap{true, false, false}, tapHold{true, false, true}, tapDouble{true, true, false},
        all{true, true, true}, holdOnly{false, false, true}, doubleOnly{false, true, false};

    // Nothing could start: the press is not ours, the game has it at once.
    {
        Classifier c;
        const auto s = c.Down(0, {}, true, timing);
        assert(s.outcome == Outcome::kNone && !s.keep && !c.Waiting());
        assert(c.Up(0.1f, timing).outcome == Outcome::kNone);
    }
    // A short press with tap only: decided at the release; a game control's press is kept until then.
    {
        Classifier c;
        assert(c.Down(0, tap, true, timing).keep && c.Waiting() && c.Keeps());  // held repeats kept too
        assert(c.Tick(0.1f, timing) == Outcome::kNone);
        const auto s = c.Up(0.15f, timing);
        assert(s.outcome == Outcome::kTap && s.keep && !c.Waiting());
    }
    // A long press with tap only: a game control's goes to the game at the hold time, and its release with it;
    // a key's does nothing.
    {
        Classifier c;
        c.Down(0, tap, true, timing);
        assert(c.Tick(0.19f, timing) == Outcome::kNone);
        assert(c.Tick(0.2f, timing) == Outcome::kGameDown && !c.Waiting() && !c.Keeps());
        const auto s = c.Up(0.5f, timing);
        assert(s.outcome == Outcome::kNone && !s.keep);
        Classifier k;
        k.Down(0, tap, false, timing);
        assert(k.Tick(0.3f, timing) == Outcome::kNone && !k.Waiting());
        assert(k.Up(0.5f, timing).outcome == Outcome::kNone);
    }
    // Hold: decided at the hold time while still down; the release stays kept.
    {
        Classifier c;
        c.Down(0, tapHold, true, timing);
        assert(c.Tick(0.2f, timing) == Outcome::kHold);
        assert(c.Up(0.6f, timing).keep);
        // Hold only, released early: the game gets the short press whole.
        Classifier h;
        h.Down(0, holdOnly, true, timing);
        assert(h.Up(0.1f, timing).outcome == Outcome::kGameTap);
    }
    // Double: two quick presses decide at the second press; a held second press is still a double.
    {
        Classifier c;
        c.Down(0, all, true, timing);
        assert(c.Up(0.1f, timing).outcome == Outcome::kNone && c.Waiting());
        assert(c.Tick(0.3f, timing) == Outcome::kNone);
        const auto s = c.Down(0.3f, all, true, timing);
        assert(s.outcome == Outcome::kDouble && s.keep);
        assert(c.Tick(2.0f, timing) == Outcome::kNone);  // held long: no hold after a double
        assert(c.Up(2.0f, timing).keep && !c.Waiting());
    }
    // Tap with double declared: the tap waits out the double window.
    {
        Classifier c;
        c.Down(0, tapDouble, false, timing);
        c.Up(0.1f, timing);
        assert(c.Tick(0.34f, timing) == Outcome::kNone);
        assert(c.Tick(0.35f, timing) == Outcome::kTap && !c.Waiting());
        // Double only, a single press: the game gets it whole once the window ends.
        Classifier d;
        d.Down(0, doubleOnly, true, timing);
        d.Up(0.1f, timing);
        assert(d.Tick(0.4f, timing) == Outcome::kGameTap);
    }
    // The actions of a hold started nothing: the game takes the press, so its release goes to the game.
    {
        Classifier c;
        c.Down(0, tapHold, true, timing);
        assert(c.Tick(0.2f, timing) == Outcome::kHold);
        c.ToGame();
        assert(!c.Up(0.4f, timing).keep);
    }
    // A load, a menu or the console drops a wait.
    {
        Classifier c;
        c.Down(0, all, true, timing);
        c.Reset();
        assert(!c.Waiting() && c.Tick(5, timing) == Outcome::kNone);
        assert(c.Up(5, timing).outcome == Outcome::kNone);
    }
    std::cout << "PressKinds ok\n";
    return 0;
}
