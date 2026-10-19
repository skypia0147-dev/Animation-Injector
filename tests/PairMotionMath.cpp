#include "runtime/PairMotionMath.h"
#include "runtime/PairTargetMath.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <limits>
#include <iostream>
using namespace AnimationInjector::PairMotionMath;
int main() {
    TranslationBasis basis;
    basis.Observe(0, 5.83687f, 0);
    assert(!basis.captured);
    basis.Observe(2.95f, 6.24706f, 0);
    assert(basis.captured && std::fabs(std::remainder(basis.yaw - 6.24706f, 2 * std::numbers::pi_v<float>)) < 0.00001f);
    const float captured = basis.yaw;
    basis.Observe(3.11f, 1.0f, 0);
    assert(basis.yaw == captured);  // Subsequent animation rotation cannot rotate the translation basis again.
    basis = {};
    basis.Observe(1, 1.8f, 0.3f);
    assert(std::fabs(basis.yaw - 1.5f) < 0.00001f);  // Rotation authored before translation is not double-counted.
    using AnimationInjector::PairTargetMath::AbsoluteHeading;
    assert(AbsoluteHeading(352) == 8 && AbsoluteHeading(-352) == 8);
    assert(AbsoluteHeading(720 + 45) == 45 && AbsoluteHeading(-720 - 45) == 45);
    assert(AbsoluteHeading(270) == 90 && AbsoluteHeading(180) == 180);
    assert(AbsoluteHeading(3600 + 91) > 90 && AbsoluteHeading(0) == 0);
    assert(AbsoluteHeading(std::numeric_limits<float>::quiet_NaN()) > 180);
    assert(AbsoluteHeading(std::numeric_limits<float>::infinity()) > 180);
    Key key;
    bool rotation;
    assert(Parse("animmotion 10 -20 3.5", 0.5f, key, rotation) && !rotation && key.y == -20);
    assert(Parse("animrotation 450", 1, key, rotation) && rotation && key.z == 450);
    assert(!Parse("animrotation nan", 1, key, rotation));
    assert(!Parse("animmotion 1 2 inf", 1, key, rotation));
    assert(!Parse("animmotion 1 2 3 junk", 1, key, rotation));
    assert(!Parse("animmotion 1 2", 1, key, rotation));
    assert(!Parse("animrotation 1", std::numeric_limits<float>::infinity(), key, rotation));
    assert(!Parse("animrotation 1", -1, key, rotation));
    std::vector<Key> keys{{2, 20, 0, 0}, {1, 8, 0, 0}, {1, 10, 0, 0}};
    Sort(keys);
    assert(keys.size() == 2 && keys[0].x == 10);
    assert(Sample(keys, 0).x == 0);
    assert(Sample(keys, 0.5f).x == 5);
    assert(Sample(keys, 1.5f).x == 15);
    assert(Sample(keys, 8).x == 20);
    std::vector<Key> turn{{1, 0, 0, 90}, {2, 0, 0, 450}};
    assert(Sample(turn, 1.5f).z == 270);
    assert(Sample({}, 5).x == 0);
    std::cout << "Pair motion parsing/interpolation checks passed\n";
}
