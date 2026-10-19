#pragma once
#include "runtime/CarryOwnership.h"
#include <cstdint>

namespace AnimationInjector::NativePairSpace {
struct Snapshot {
    CarryOwnership::Scene region;
    // Identity only. Never dereference a retained engine physics world.
    std::uintptr_t physics = 0;
};
constexpr bool Compatible(Snapshot a, Snapshot b) {
    return a.region.interior == b.region.interior && a.region.world == b.region.world &&
           CarryOwnership::SameScene(a.region, b.region) && a.physics && a.physics == b.physics;
}
}
