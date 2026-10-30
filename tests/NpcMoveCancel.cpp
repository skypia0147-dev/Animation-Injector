#include "runtime/NpcMoveCancel.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>

// An NPC's move cancel ends its action on the first request its AI sends after getting movement back (10-30).
int main() {
    using AnimationInjector::NpcMoveCancel::IsRequest;
    // What the AI sent in the in-game check, and the other requests that end it.
    for (const char* e : {"moveStart", "attackStart", "attackPowerStartForward", "attackStartLeftHand", "bashStart",
             "blockStart", "sprintStart", "MOVESTART", "AttackStart"})
        assert(IsRequest(e));
    // What the engine sends around them without asking to do anything: a turn, the clip's own stop, blends, the
    // action's own events.
    for (const char* e : {"TurnRight", "turnStop", "moveStop", "CyclicCrossBlend", "CyclicFreeze", "blockStop",
             "AI_MyAnimationSet_hw_N_BlockHit_5_Recover", "moveStartX", "attac", ""})
        assert(!IsRequest(e));
    std::cout << "NpcMoveCancel ok\n";
    return 0;
}
