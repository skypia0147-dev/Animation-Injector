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
            const bool isPlayer = sender == player;
            check(Accepts(EventFrom::kAny, sender, isPlayer));
            check(Accepts(EventFrom::kInitiator, sender, isPlayer) == (sender == 0));
            check(Accepts(EventFrom::kReceiver, sender, isPlayer) == (sender == 1));
            check(Accepts(EventFrom::kPlayer, sender, isPlayer) == isPlayer);
            check(Accepts(EventFrom::kNpc, sender, isPlayer) == !isPlayer);
            check(Targets(EventTarget::kSender, sender, player) == (1u << sender));
            check(Targets(EventTarget::kPartner, sender, player) == (1u << (1 - sender)));
            check(Targets(EventTarget::kBoth, sender, player) == 3);
            check(Targets(EventTarget::kInitiator, sender, player) == 1);
            check(Targets(EventTarget::kReceiver, sender, player) == 2);
            check(Targets(EventTarget::kPlayer, sender, player) == (player < 0 ? 0u : 1u << player));
        }
    // Outside a pair: only the player and the sender itself can be reached (the executor handles those).
    check(!Accepts(EventFrom::kInitiator, -1, false) && !Accepts(EventFrom::kReceiver, -1, true));
    check(Accepts(EventFrom::kPlayer, -1, true) && Accepts(EventFrom::kNpc, -1, false));
    check(Targets(EventTarget::kPartner, -1, 0) == 0 && Targets(EventTarget::kSender, -1, 0) == 0);
    // Clip-time effects: the union over the senders `from` accepts.
    check(TimedTargets(EventFrom::kReceiver, EventTarget::kSender, 0) == 2);
    check(TimedTargets(EventFrom::kReceiver, EventTarget::kPartner, 0) == 1);
    check(TimedTargets(EventFrom::kAny, EventTarget::kSender, 0) == 3);
    check(TimedTargets(EventFrom::kAny, EventTarget::kBoth, 0) == 3);
    check(TimedTargets(EventFrom::kPlayer, EventTarget::kSender, 1) == 2);
    check(TimedTargets(EventFrom::kPlayer, EventTarget::kBoth, -1) == 0);
    check(TimedTargets(EventFrom::kNpc, EventTarget::kSender, 0) == 2);
    std::cout << "Event function from/target matrix and clip-time deduplication passed\n";
}
