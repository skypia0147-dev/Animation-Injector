#include "runtime/HeldKeys.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <iostream>

// The keys the player holds and the held time of the key that started the action (10-30, charge).
int main() {
    using namespace AnimationInjector::HeldKeys;
    const auto near = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
    Tracker t;
    const Key g{Source::kKey, 34}, attack{Source::kControl, 7}, pad{Source::kPad, 0x1000};
    assert(!t.Held(g) && t.ChainHeldTime(1) < 0);  // nothing yet
    t.Press(g, 1.0f);
    t.Press(pad, 1.0f);
    assert(t.Held(g) && t.Held(pad) && !t.Held(attack));
    t.Press(g, 1.5f);  // a repeat: still held
    assert(t.Held(g));
    t.SetChain(g, 1.0f);
    assert(t.Chain() && *t.Chain() == g);
    assert(near(t.ChainHeldTime(1.8f), 0.8f));  // still down: until now
    t.Release(g, 2.9f);
    assert(!t.Held(g) && t.Held(pad));
    assert(near(t.ChainHeldTime(5.0f), 1.9f));  // up: until its release
    // A start by a press of a key that went down before (a hold) counts from that press.
    t.Press(attack, 6.0f);
    t.SetChain(attack, 6.0f);
    t.Release(attack, 6.4f);
    assert(near(t.ChainHeldTime(9.0f), 0.4f));
    // A release seen before the press that started the action (an old one) does not end it.
    t.SetChain(attack, 7.0f);
    assert(near(t.ChainHeldTime(7.5f), 0.5f));
    t.Reset();
    assert(!t.Chain() && !t.Held(pad) && t.ChainHeldTime(10) < 0);
    std::cout << "HeldKeys ok\n";
    return 0;
}
