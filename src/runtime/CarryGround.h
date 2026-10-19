#pragma once

namespace RE {
class Actor;
class NiPoint3;
}

namespace AnimationInjector::CarryGround {
// Where a carry places a released target: `out` is the point 80 units in front of the actor, moved
// onto static ground or terrain (never an actor) found between 60 above and 180 below it. false:
// no such ground (out stays the point in front).
bool InFront(RE::Actor* actor, RE::NiPoint3& out);
}
