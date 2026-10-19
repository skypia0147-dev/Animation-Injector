#include "runtime/ExitBlendMailbox.h"
#include "core/PairRootExitPolicy.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
#include <limits>

using Mailbox = AnimationInjector::ExitBlendMailbox<unsigned, 2>;
int main() {
    const Mailbox::Key player{1, 1}, partner{1, 2}, entry{2, 1};
    Mailbox box;
    std::array<unsigned, 2> requests{};
    // The same effect in different character clones must not overwrite its owner.
    assert(box.Activate(player, 11));
    assert(box.Activate(partner, 22));
    assert(!box.Activate(entry, 33));
    box.Observe(entry, .01f, true);
    assert(box.Take(requests) == 0);
    box.Observe(player, .01f, true);
    box.Observe(player, .01f, true);
    assert(box.Take(requests) == 1 && requests[0] == 11);
    assert(box.Take(requests) == 0);
    box.Observe(partner, .01f, true);
    box.Deactivate(partner);
    assert(box.Take(requests) == 0);
    // Reusing an effect after a new playback discards its old pending token.
    box.Observe(player, .01f, true);
    assert(box.Activate(player, 44));
    assert(box.Take(requests) == 0);
    box.Observe(player, .01f, true);
    assert(box.Take(requests) == 1 && requests[0] == 44);
    box.Observe(player, .01f, true);
    box.Reset();
    assert(box.Take(requests) == 0);
    box.Observe(player, .01f, true);
    assert(box.Take(requests) == 0);
    // The held count the hooks read without the lock (code review 2026-09-30 R-18): none after a reset,
    // one per activation, back down on deactivation; a Take keeps the item.
    assert(box.Active() == 0);
    assert(box.Activate(player, 55) && box.Active() == 1);
    assert(box.Activate(player, 56) && box.Active() == 1);  // the same key replaces its item
    assert(box.Activate(partner, 57) && box.Active() == 2);
    box.Observe(player, .01f, true);
    assert(box.Take(requests) == 1 && box.Active() == 2);
    box.Deactivate(player);
    assert(box.Active() == 1);
    box.Reset();
    assert(box.Active() == 0);

    // Hold before exit, press during exit, or never press: authored time is identical.
    for (const float duration : {.2f, .6f, 2.0f}) {
        for (int pressAt : {0, 5, 1000}) {
            box.Reset();
            box.Activate(player, 55);
            float elapsed = 0;
            int firstRequest = -1, frames = 0;
            while (elapsed + .00001f < duration) {
                const bool held = frames >= pressAt;
                const float delta = box.Observe(player, .01f, held);
                assert(delta == .01f);
                elapsed += delta;
                if (box.Take(requests) && firstRequest < 0) firstRequest = frames;
                ++frames;
            }
            assert(std::fabs(elapsed - duration) < .00001f);
            assert(firstRequest == (pressAt < frames ? pressAt : -1));
        }
    }
    box.Reset();
    box.Activate(player, 66);
    box.Observe(player, 0, true);
    box.Observe(player, -1, true);
    box.Observe(player, std::numeric_limits<float>::quiet_NaN(), true);
    box.Observe(player, std::numeric_limits<float>::infinity(), true);
    assert(box.Take(requests) == 0);
    using AnimationInjector::PairRootExitMayContinue;
    // A walking/turning player used to abort root preservation; a stationary partner
    // must still retain its guard. Input cannot bypass identity/timeout checks.
    assert(!PairRootExitMayContinue(true, false, 8, .2f, .1f, .6f));
    assert(PairRootExitMayContinue(true, true, 8, .2f, .1f, .6f));
    assert(PairRootExitMayContinue(true, false, 0, 0, .1f, .6f));
    assert(!PairRootExitMayContinue(false, true, 8, .2f, .1f, .6f));
    assert(!PairRootExitMayContinue(true, true, 8, .2f, .7f, .6f));
    assert(!PairRootExitMayContinue(true, true, 8, std::numeric_limits<float>::quiet_NaN(), .1f, .6f));
    std::cout
        << "Exit blend movement: ownership, stale requests, load reset, late input, preserved durations and moving root preservation guards passed\n";
}
