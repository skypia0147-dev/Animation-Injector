#include "runtime/ActorImmunity.h"
#include "runtime/LifecyclePolicy.h"
#include "runtime/PairedCleanup.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
using namespace AnimationInjector;
int main() {
    PairedCleanup::Handoff handoff;
    assert(handoff.ProtectedDestination(0) == -1);
    handoff.destination = {3, 4};
    handoff.open = true;
    assert(handoff.ProtectedDestination(0) == 3 && handoff.ProtectedDestination(1) == 4);
    assert(handoff.ProtectedDestination(-1) == -1 && handoff.ProtectedDestination(2) == -1);
    handoff.Close(0, true, false);
    assert(handoff.ProtectedDestination(0) == 3 && handoff.ProtectedDestination(1) == -1);
    handoff = {};
    assert(handoff.ProtectedDestination(0) == -1);

    // IFrame immunity is the actor's own ghost state (0.12.230): two actors, even of one base, lease
    // separately, and only the last release of an actor turns it back.
    ActorImmunity::Ledger ledger;
    constexpr RE::FormID guardA = 0x100, guardB = 0x101, ghostNpc = 0x102;
    assert(ledger.Acquire(guardA, false));
    assert(!ledger.Acquire(guardA, false));  // a second holder on the same actor
    assert(ledger.Holders(guardA) == 2 && ledger.Holders(guardB) == 0);
    assert(ledger.Acquire(guardB, false));  // another actor is its own lease
    assert(!ledger.Release(guardA));        // one holder is left
    assert(ledger.Release(guardA));         // the last one turns it back
    assert(!ledger.Release(guardA));        // released once
    assert(ledger.Release(guardB));
    assert(!ledger.Acquire(0, false) && !ledger.Release(0) && ledger.Size() == 0);
    // An actor that was a ghost before (a ghost NPC, a quest's SetGhost) is never touched.
    assert(!ledger.Acquire(ghostNpc, true));
    assert(!ledger.Release(ghostNpc) && ledger.Size() == 0);

    // A save written during a lease holds the actor as a ghost: the co-save's leases turn it back after
    // the load, once.
    ledger.Acquire(guardA, false);
    ledger.Acquire(ghostNpc, true);
    auto saved = ledger.Snapshot();
    assert(saved.size() == 2 && saved[0].actor == guardA && !saved[0].wasGhost && saved[1].wasGhost);
    ledger.Release(guardA);  // every holder lets go before the load
    ledger.Release(ghostNpc);
    assert(ledger.Clear().empty() && ledger.Size() == 0);
    auto back = ledger.Restore(saved);
    assert(back.size() == 1 && back[0] == guardA);  // the ghost NPC stays a ghost
    // Leased again before PostLoad: it stays a ghost, and its release restores the saved state.
    ledger.Acquire(guardA, true);  // the loaded actor reads as a ghost now
    assert(ledger.Restore(saved).empty());
    assert(ledger.Release(guardA));  // turned back: it was not a ghost when the save was written
    // A lease its holder did not release before a load is released there.
    ledger.Acquire(guardA, false);
    ledger.Acquire(ghostNpc, true);
    back = ledger.Clear();
    assert(back.size() == 1 && back[0] == guardA && ledger.Size() == 0);
    assert(!ledger.Release(guardA));

    bool owned = true, graph = false;
    int writes = 0;
    auto read = [&](bool& value) {
        value = graph;
        return true;
    };
    auto write = [&](bool value) {
        graph = value;
        ++writes;
    };
    // A locked action that started while the graph allowed rotation (e.g. combat) must not
    // write that true back after its exit: locomotion would stay in rotation-only mode.
    HandBackRotation(owned, false, read, write);
    assert(!graph && !owned && writes == 0);
    HandBackRotation(owned, false, read, write);
    assert(writes == 0);  // released once
    owned = true;
    graph = true;
    HandBackRotation(owned, false, read, write);
    assert(graph && !owned && writes == 0);  // Destination graph set it; it takes precedence.
    owned = true;
    graph = true;
    HandBackRotation(owned, true, read, write);
    assert(!graph && !owned && writes == 1);  // Our own open rotation window is closed.
    owned = true;
    graph = false;
    HandBackRotation(owned, true, read, write);
    assert(!graph && !owned && writes == 1);  // Already cleared by the destination: no write.
    owned = true;
    graph = true;
    HandBackRotation(owned, true, [](bool&) { return false; }, write);
    assert(graph && !owned && writes == 1);  // Unreadable graph: leave it alone.
    writes = 0;
    // Recovery leaves the animation rotation mode as well as animation driving.
    auto acceptedWrite = [&](bool value) {
        write(value);
        return true;
    };
    const int rotationWrites = writes;
    for (int frame = 0; frame < 8; ++frame) {
        graph = true;
        MaintainRecoveredRotation(true, read, acceptedWrite);
        assert(!graph && writes == rotationWrites + frame + 1 && !owned);
    }
    const int stableWrites = writes;
    MaintainRecoveredRotation(true, read, acceptedWrite);
    assert(writes == stableWrites);  // Already motion-driven: no redundant write.
    graph = true;
    MaintainRecoveredRotation(false, read, acceptedWrite);  // New owner, death or idle.
    MaintainRecoveredRotation(true, [](bool&) { return false; }, acceptedWrite);
    MaintainRecoveredRotation(true, read, [](bool) { return false; });
    assert(graph && writes == stableWrites);
    MaintainRecoveredRotation(true, read, acceptedWrite);  // Retry after failed write.
    assert(!graph && writes == stableWrites + 1);
    MaintainRecoveredRotation(false, read, acceptedWrite);  // Finish never re-enables animation mode.
    assert(!graph && writes == stableWrites + 1);
    assert(ExitWatchdog(2) == 3 && ExitWatchdog(5) == 6 && ExitWatchdog(-1) == 6);

    enum State { Active, Exiting, Idle };
    State state = Active;
    bool exitSent = false;
    int drivenWrites = 0;
    auto prepare = [&] {
        state = Exiting;
        exitSent = true;
    };
    auto pending = [&] { return state == Exiting; };
    auto accepted = [&] { ++drivenWrites; };
    auto rejected = [&] {
        state = Active;
        exitSent = false;
    };
    assert(!RequestPreparedExit(prepare, [] { return false; }, pending, accepted, rejected));
    assert(state == Active && !exitSent && drivenWrites == 0);
    assert(RequestPreparedExit(
        prepare,
        [&] {
            state = Idle;
            exitSent = false;
            return true;
        },
        pending, accepted, rejected));
    assert(state == Idle && !exitSent && drivenWrites == 0);
    state = Active;
    assert(RequestPreparedExit(prepare, [] { return true; }, pending, accepted, rejected));
    assert(state == Exiting && exitSent && drivenWrites == 1);
    std::cout
        << "Lifecycle: shared immunity, restoration, interrupted ownership, exit rejection/synchronous completion passed\n";
}
