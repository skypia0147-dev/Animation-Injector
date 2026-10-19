#include "runtime/CarryContinuity.h"
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace AnimationInjector::CarryContinuity;
void Check(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
int main() {
    HitMailbox mailbox;
    mailbox.Arm(0x14, 0x100);
    for (unsigned frame = 0; frame < 60 * 60 * 8; ++frame)
        Check(!mailbox.Consume(0) && !mailbox.Consume(1), "No lifetime limit or implicit stagger interruption");
    const auto first = mailbox.Watch(0x14), target = mailbox.Watch(0x100);
    Check(!mailbox.Watch(0x999), "Unrelated actor not watched");
    mailbox.Hit(target);
    Check(!mailbox.Consume(0) && mailbox.Consume(1) && !mailbox.Consume(1), "Target damage consumed once");
    mailbox.Hit(first);
    Check(mailbox.Consume(0), "Carrier damage consumed");
    mailbox.Reset();
    mailbox.Hit(first);
    Check(!mailbox.Consume(0), "Cancelled/load receipt ignored");
    mailbox.Arm(0x14, 0x100);
    mailbox.Hit(first);
    Check(!mailbox.Consume(0), "Previous carry cannot interrupt reused actors");
    Check(!mailbox.Owns(first) && mailbox.Owns(mailbox.Watch(0x14)), "Cell callbacks are generation-bound");
    const auto current = mailbox.Watch(0x14);
    mailbox.Hit(current);
    Check(mailbox.Consume(0), "Hit event requires no health comparison");
    TransferGate gate;
    gate.Destination(10);
    for (unsigned i = 0; i < 1000; ++i)
        Check(!gate.Tick(1, true, true) && !gate.Tick(1, false, false),
            "Loading and missing destination do not time out");
    Check(gate.readyAge == 0, "No wall-clock timer");
    Check(gate.CanMove(), "First cell transfer permitted");
    gate.Moved();
    Check(!gate.CanMove(), "No per-frame MoveTo spam");
    gate.Tick(.5f, false, true);
    Check(gate.CanMove(), "Bounded retry available");
    gate.Moved();
    gate.Tick(.5f, false, true);
    gate.Moved();
    gate.Tick(.5f, false, true);
    Check(!gate.CanMove(), "Three failed relocations are bounded");
    Check(!gate.Ready(true) && !gate.Ready(false) && !gate.Ready(true) && gate.Ready(true),
        "Two stable readiness samples required");
    gate.Destination(11);
    Check(gate.CanMove() && gate.readyAge == 0 && gate.stable == 0, "Another door resets destination attempt");
    Check(!gate.Tick(0, false, true) && !gate.Tick(-1, false, true), "Invalid delta does not advance");
    Check(gate.Tick(15, false, true), "Failed reconstruction restores control after active-world wait");
    gate = {};
    Check(!gate.CanMove(), "No MoveTo until a destination exists");
    std::cout
        << "Carry continuity: hit events without HP polling, both roles, stale receipts/load reset, loading pause, move retries and reconstruction gate passed\n";
}
