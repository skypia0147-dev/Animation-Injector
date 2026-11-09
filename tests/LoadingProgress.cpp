#ifdef NDEBUG
#undef NDEBUG
#endif
#include "core/LoadingProgressModel.h"
#include <cassert>
#include <iostream>
#include <limits>

using namespace AnimationInjector::LoadingProgress;
int main() {
    Model model;
    assert(!model.Read().visible);
    const auto first = model.Begin(Phase::kAnimation);
    assert(model.Read().visible && model.Read().pending == 1 && model.Read().alpha == 1);
    assert(model.Finish(first, Outcome::kReady));
    assert(!model.Finish(first, Outcome::kReady));
    auto state = model.Advance(60);  // Completion before first UI frame must still appear.
    assert(state.visible && state.alpha == 1 && state.ready == 1 && state.Fraction() == 1);
    for (int i = 0; i < 11; ++i) state = model.Advance(.1f);
    assert(!state.visible);
    assert(!model.ObservesNativeRequests());
    assert(model.Begin(Phase::kAnimation) == 0 && model.Begin(Phase::kRegistration) == 0);
    assert(model.BeginNativeAnimation() == 0);
    for (int i = 0; i < 100; ++i) model.Advance(.1f);
    assert(!model.Read().visible && model.Read().total == 1);

    model.Reset();
    const auto failure = model.Begin(Phase::kAnimation);
    const auto cancelled = model.Begin(Phase::kAnimation);
    const auto success = model.Begin(Phase::kRegistration);
    model.Finish(failure, Outcome::kFailed);
    model.Finish(cancelled, Outcome::kCancelled);
    model.Finish(success, Outcome::kReady);
    state = model.Advance(0);
    assert(state.failed == 1 && state.ready == 1 && state.cancelled == 1 && state.Fraction() == .5f);
    for (int i = 0; i < 20; ++i) state = model.Advance(.1f);
    assert(state.visible);  // Failures get time to be read, without pretending 100% success.
    for (int i = 0; i < 15; ++i) state = model.Advance(.1f);
    assert(!state.visible);

    model.Reset();
    const auto old = model.Begin(Phase::kAnimation);
    model.Reset();
    assert(!model.Read().visible && model.Read().total == 0);
    const auto fresh = model.Begin(Phase::kAnimation);
    assert(fresh != old && !model.Finish(old, Outcome::kFailed));
    assert(model.Read().pending == 1 && model.Read().failed == 0);
    for (int i = 0; i < 200; ++i) state = model.Advance(10);
    assert(state.visible && state.alpha == 1);  // A pending job never fades during I/O or retry delay.
    model.Finish(fresh, Outcome::kReady);
    model.Advance(0);
    for (int i = 0; i < 8; ++i) model.Advance(.1f);
    assert(model.Read().alpha < 1);
    const auto next = model.Begin(Phase::kAnimation);
    assert(model.Read().visible && model.Read().alpha == 1);
    model.Finish(next, Outcome::kReady);
    state = model.Advance(0);
    model.Advance(std::numeric_limits<float>::quiet_NaN());
    model.Advance(-1);
    assert(model.Read().alpha == state.alpha);

    model.Reset();
    for (int i = 0; i < 10000; ++i) model.Finish(model.Begin(Phase::kRegistration), Outcome::kReady);
    assert(model.Read().total == 10000 && model.Read().ready == 10000 && model.Read().pending == 0);
    assert(model.Advance(1).visible);  // Synchronous registration must not be lost either.

    // A long loading screen must not consume the only display before the
    // character's real animation jobs begin after PostLoadGame.
    model.Reset(true);
    const auto registered = model.Begin(Phase::kRegistration);
    model.Finish(registered, Outcome::kReady);
    for (int i = 0; i < 600; ++i) model.Advance(.1f);
    assert(model.Read().visible && model.ObservesNativeRequests());
    model.GameLoaded();
    assert(!model.ObservesNativeRequests() && model.BeginNativeAnimation() == 0);
    for (int i = 0; i < 600; ++i) model.Advance(.1f, false);  // Still in loading UI/world setup.
    assert(model.Read().visible && model.Read().alpha == 1);
    const auto preloaded = model.Begin(Phase::kAnimation);
    assert(preloaded && model.Read().pending == 1);
    for (int i = 0; i < 600; ++i) model.Advance(.1f);
    assert(model.Read().visible && model.Read().alpha == 1);
    model.Finish(preloaded, Outcome::kReady);
    model.Advance(0);
    for (int i = 0; i < 11; ++i) model.Advance(.1f);
    assert(!model.Read().visible && model.Read().ready == 2);
    // Loading-menu changes, late graph registration and gameplay requests
    // cannot reopen a finished session, even after another PostLoad callback.
    model.GameLoaded();
    for (int i = 0; i < 10; ++i) {
        model.Advance(.1f, false);
        model.Advance(.1f, true);
        assert(model.Begin(Phase::kRegistration) == 0);
        assert(model.Begin(Phase::kAnimation) == 0);
        assert(model.BeginNativeAnimation() == 0);
    }
    assert(!model.Read().visible && model.Read().total == 2);

    // No initial work must also disarm the display. Otherwise the first
    // action much later in gameplay would incorrectly become the initial load.
    model.Reset();
    model.GameLoaded();
    for (int i = 0; i < 12; ++i) {
        model.Advance(.1f);
        assert(!model.Read().visible);
    }
    assert(model.Begin(Phase::kAnimation) == 0 && model.Read().total == 0);
    model.Reset(true);
    const auto nextSave = model.BeginNativeAnimation();
    assert(nextSave && model.Read().visible && model.Read().pending == 1);
    assert(!model.Finish(preloaded, Outcome::kReady));
    model.Finish(nextSave, Outcome::kReady);
    model.GameLoaded();
    model.Advance(0);
    for (int i = 0; i < 11; ++i) model.Advance(.1f);
    assert(!model.Read().visible && model.Read().ready == 1);

    // Engine limits: the fuller report wins, a shortfall sticks, both survive new batches and reloads.
    Model usage;
    assert(!usage.Read().nodes.Known() && !usage.Read().animations.Known());
    usage.ReportNodes(4211, 65535, false);  // first person
    usage.ReportNodes(7549, 65535, false);  // third person, fuller
    usage.ReportNodes(4211, 65535, false);
    assert(usage.Read().nodes.used == 7549 && !usage.Read().nodes.Warn() && !usage.Read().nodes.shortfall);
    const auto before = usage.Read().revision;
    usage.ReportNodes(7549, 65535, false);
    assert(usage.Read().revision == before);  // nothing new, nothing to redraw
    usage.ReportAnimations(29900, 32767, false);
    assert(usage.Read().animations.Warn());
    usage.ReportAnimations(16333, 32767, true);
    assert(usage.Read().animations.used == 29900 && usage.Read().animations.shortfall);
    usage.ReportNodes(1, 0, true);  // no limit: ignored
    assert(!usage.Read().nodes.shortfall);
    // A graph past the node ids that gets renumbered reports its state machine ids; the fuller row is shown.
    Model renumbered;
    renumbered.ReportNodes(4211, 65535, false);                       // first person, fits
    renumbered.ReportNodes(72330, 65535, false, 9290 + 4096, 32767);  // third person, renumbered: fuller
    {
        const auto nodes = renumbered.Read().nodes;
        assert(nodes.Extended() && nodes.used == 72330 && nodes.machinesUsed == 13386 && !nodes.shortfall);
        assert(!nodes.Warn());  // judged by the state machine ids, not by 72330 of 65535
    }
    renumbered.ReportNodes(60000, 65535, false);  // a smaller graph does not take the row
    assert(renumbered.Read().nodes.Extended() && renumbered.Read().nodes.used == 72330);
    renumbered.ReportNodes(73000, 65535, true);  // a fuller graph that could not be renumbered does
    assert(!renumbered.Read().nodes.Extended() && renumbered.Read().nodes.shortfall);
    const auto job = usage.Begin(Phase::kAnimation);
    assert(usage.Read().nodes.used == 7549 && usage.Read().animations.shortfall);
    usage.Finish(job, Outcome::kReady);
    state = usage.Advance(0);
    for (int i = 0; i < 70; ++i) state = usage.Advance(.1f);
    assert(state.visible);  // a shortfall stays up to be read
    for (int i = 0; i < 20; ++i) state = usage.Advance(.1f);
    assert(!state.visible);
    usage.Reset(true);
    assert(usage.Read().nodes.used == 7549 && usage.Read().animations.used == 29900);
    std::cout
        << "Loading progress: once per initial load, completion fence, empty session, gameplay suppression, immediate short display, reload and engine limit rows passed\n";
}
