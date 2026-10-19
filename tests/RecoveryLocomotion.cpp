#include "runtime/RecoveryLocomotion.h"
#include "runtime/PairLocomotionStart.h"
#include "runtime/LocomotionDestinationWindow.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
#include <limits>
#include <vector>

int main() {
    using namespace AnimationInjector;
    struct Owner {
        int actor = 0, generation = 0, request = 0;
        bool operator==(const Owner&) const = default;
    };
    LocomotionDestinationWindow<Owner> pending;
    const Owner first{14, 1, 1}, next{14, 1, 2}, loaded{14, 2, 1}, npc{42, 1, 1};
    pending.Arm(first);
    pending.Tick(0.02f);  // Activation can occur after the event callback returned.
    assert(!pending.Consume(npc, 10) && !pending.Consume(next, 10));
    assert(pending.Consume(first, 10));
    assert(!pending.Consume(first, 10));
    assert(pending.Consume(first, 20));  // Separate player graph contexts.
    pending.Arm(first);                  // A duplicate event cannot reset consumption or extend life.
    assert(!pending.Consume(first, 10));
    pending.Tick(0.34f);
    assert(!pending.Consume(first, 30));
    pending.Arm(next);
    assert(!pending.Consume(first, 30));
    pending.Tick(std::numeric_limits<float>::quiet_NaN());
    assert(pending.Consume(next, 30));
    pending.Reset();
    assert(!pending.Consume(next, 40));
    pending.Arm(loaded);
    assert(!pending.Consume(first, 40) && pending.Consume(loaded, 40));
    pending.Reset();
    std::cout
        << "Destination window: deferred callbacks, context deduplication, actor/request/load isolation and expiry passed\n";
    const std::array<LocomotionStartState, 2> states{{{0, "MT_Standing_State"}, {1, "MT_LocomotionType_State"}}};
    assert(PairLocomotionStart(true, true, true, "MT_Default_Behavior", 0, states) == 1);
    assert(!PairLocomotionStart(false, true, true, "MT_Default_Behavior", 0, states));
    assert(!PairLocomotionStart(true, false, true, "MT_Default_Behavior", 0, states));
    assert(!PairLocomotionStart(true, true, false, "MT_Default_Behavior", 0, states));
    assert(!PairLocomotionStart(true, true, true, "Other", 0, states));
    assert(!PairLocomotionStart(true, true, true, "MT_Default_Behavior", 5, states));
    auto replaced = states;
    replaced[1].name = "DifferentModState";
    assert(!PairLocomotionStart(true, true, true, "MT_Default_Behavior", 0, replaced));
    auto relocated = states;
    relocated[0].id = 20;
    relocated[1].id = 40;
    assert(PairLocomotionStart(true, true, true, "MT_Default_Behavior", 20, relocated) == 40);
    int declaration = 0, active = -1;
    ActivateLocomotionStart(declaration, 1, [&] {
        active = declaration;
        ActivateLocomotionStart(declaration, 2, [&] { assert(declaration == 2); });
        assert(declaration == 1);
    });
    assert(active == 1 && declaration == 0);
    try {
        ActivateLocomotionStart(declaration, 1, [] { throw 42; });
    } catch (int) {
    }
    assert(declaration == 0);
    std::cout
        << "Pair destination: native moving start, ordinary/template/unmatched exclusions, non-hardcoded IDs and scoped restoration passed\n";
    using AnimationInjector::RecoveryLocomotion;
    RecoveryLocomotion state;
    std::vector<bool> events;
    // Regression: the native movement-start request was rejected by sync, but
    // physical movement stayed active. The graph must leave idle exactly once.
    bool graphMoving = false;
    auto send = [&](bool moving) {
        events.push_back(moving);
        graphMoving = moving;
        return true;
    };
    state.Update(true, true, 0, false, send);
    assert(events.empty());
    for (int frame = 0; frame < 300; ++frame) state.Update(true, true, 364, false, send);
    assert(graphMoving && events.size() == 1 && events[0]);
    // Repeated maintenance in a frame cannot restart the walking animation.
    for (int frame = 0; frame < 30; ++frame) state.Update(true, true, 0, false, send);
    assert(!graphMoving && events.size() == 2 && !events[1]);
    state.Update(true, true, 80, false, send);
    assert(graphMoving && events.size() == 3);
    // Pause, death, new owner, missing data and bad samples cannot send events.
    state.Update(false, true, 0, false, send);
    state.Update(true, false, 0, false, send);
    state.Update(true, true, -1, false, send);
    state.Update(true, true, std::numeric_limits<float>::quiet_NaN(), false, send);
    state.Update(true, true, std::numeric_limits<float>::infinity(), false, send);
    assert(events.size() == 3 && graphMoving);
    // Pair completion/load discards the tracker without forcing idle. After the
    // scene is gone, the engine resumes responsibility for future stop events.
    state = {};
    state.Update(false, true, 0, false, send);
    assert(graphMoving && events.size() == 3);
    // A subsequent pickup is independent, even when its movement key is held.
    state.Update(true, true, 150, false, send);
    assert(events.size() == 4 && events.back());
    state = {};
    unsigned attempts = 0;
    state.Update(true, true, 120, false, [&](bool) {
        ++attempts;
        return false;
    });
    assert(!state.moving && attempts == 1);
    state.Update(true, true, 120, false, send);
    assert(state.moving && events.size() == 5);
    // Regression: the controller reports zero speed for about 150 ms after
    // recovery. Prepare the destination on intent, not the first positive speed.
    for (int fps : {30, 60, 120, 144}) {
        state = {};
        events.clear();
        graphMoving = false;
        state.Update(false, true, 0, true, send);
        state.Update(true, false, 0, true, send);
        state.Update(true, true, -1, true, send);
        state.Update(true, true, std::numeric_limits<float>::quiet_NaN(), true, send);
        assert(events.empty());
        state.Update(true, true, 0, true, send);
        assert(graphMoving && events.size() == 1);
        for (int frame = 0; frame < fps / 6; ++frame) state.Update(true, true, 0, true, send);
        state.Update(true, true, 14.568f, true, send);
        state.Update(true, true, 364, true, send);
        assert(events.size() == 1 && graphMoving);
        // Releasing the key during deceleration must not snap back to idle.
        state.Update(true, true, 100, false, send);
        assert(events.size() == 1);
        state.Update(true, true, 0, false, send);
        assert(events.size() == 2 && !graphMoving);
        // A short tap that ends before acceleration still has a matching stop.
        state.Update(true, true, 0, true, send);
        state.Update(true, true, 0, false, send);
        assert(events.size() == 4 && !graphMoving);
    }
    state = {};
    events.clear();
    state.Update(true, true, 0, true, [](bool) { return false; });
    assert(!state.moving);
    state.Update(true, true, 0, true, send);
    assert(events.size() == 1 && state.moving);
    // Completion/load does not synthesize a stop after priming locomotion.
    state = {};
    state.Update(false, true, 0, false, send);
    assert(events.size() == 1);
    std::cout
        << "RecoveryLocomotion: speed fallback, intent before acceleration at 30/60/120/144 FPS, short tap, deceleration, duplicate maintenance, eligibility/data guards, completion/load and failed-send retry passed\n";
}
