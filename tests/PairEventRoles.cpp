#include "runtime/PairEventRoles.h"
#include <cstdlib>
#include <iostream>
using namespace AnimationInjector;
void check(bool v) {
    if (!v) std::abort();
}
int main() {
    using namespace PairEventRoles;
    for (int player : {-1, 0, 1})
        for (int sender : {0, 1}) {
            check(Targets(EventTarget::kSender, sender, player) == (1u << sender));
            check(Targets(EventTarget::kBoth, sender, player) == 3);
            check(Targets(EventTarget::kInitiator, sender, player) == 1);
            check(Targets(EventTarget::kReceiver, sender, player) == 2);
            check(Targets(EventTarget::kPlayer, sender, player) == (player < 0 ? 0u : 1u << player));
        }
    // Outside a pair: only the player and the sender itself can be reached (the executor handles those).
    check(Targets(EventTarget::kSender, -1, 0) == 0 && Targets(EventTarget::kBoth, -1, 0) == 0);
    // Clip-time effects: both roles count as the sender, once each.
    check(TimedTargets(EventTarget::kBoth, 0) == 3);
    check(TimedTargets(EventTarget::kBoth, -1) == 3);
    check(TimedTargets(EventTarget::kInitiator, 1) == 1);
    check(TimedTargets(EventTarget::kReceiver, 0) == 2);
    check(TimedTargets(EventTarget::kPlayer, 1) == 2);
    check(TimedTargets(EventTarget::kPlayer, -1) == 0);
    std::cout << "Event function target matrix and clip-time deduplication passed\n";
}
