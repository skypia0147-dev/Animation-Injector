#include "runtime/TdmTurn.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <iostream>

// Turning the player in TDM's stead while an action's Rotate window is on (10-31).
int main() {
    using namespace AnimationInjector::TdmTurn;
    using Mode = AnimationInjector::TdmMovement::Mode;
    constexpr float pi = 3.14159265358979f;
    const auto near = [](float a, float b) { return std::fabs(std::remainder(a - b, 2 * pi)) < 1e-4f; };
    const float camera = 0.4f;
    // Vanilla style: any movement input faces the camera's front.
    for (const auto& [x, y] : {std::pair{0.0f, 1.0f}, {1.0f, 0.0f}, {-1.0f, -1.0f}})
        assert(near(Target(Mode::kVanillaStyle, camera, x, y), camera));
    // Directional: the input's way from the camera (right a quarter turn clockwise, back half a turn, a diagonal).
    assert(near(Target(Mode::kDirectional, camera, 0, 1), camera));
    assert(near(Target(Mode::kDirectional, camera, 1, 0), camera + pi / 2));
    assert(near(Target(Mode::kDirectional, camera, 0, -1), camera + pi));
    assert(near(Target(Mode::kDirectional, camera, -1, 1), camera - pi / 4));
    // No input, no TDM, TDM off, a locked target: no turn.
    assert(std::isnan(Target(Mode::kVanillaStyle, camera, 0.1f, 0.1f)));
    for (Mode m : {Mode::kAbsent, Mode::kDisabled, Mode::kTargetLock}) assert(std::isnan(Target(m, camera, 0, 1)));
    // Eased: part of the turn in a frame, the rest when the frame would pass it.
    assert(std::fabs(Step(1.0f, 1.0f / 60) - 0.2f) < 1e-4f);
    assert(Step(1.0f, 0.2f) == 1.0f && Step(-0.5f, 1.0f) == -0.5f && Step(1.0f, -1.0f) == 0.0f);
    // Root motion turned as the body was: forward after a quarter turn clockwise goes right, and back by half a turn.
    float x = 0, y = 10;
    Steer(pi / 2, x, y);
    assert(std::fabs(x - 10) < 1e-4f && std::fabs(y) < 1e-4f);
    x = 3, y = 4;
    Steer(pi, x, y);
    assert(std::fabs(x + 3) < 1e-4f && std::fabs(y + 4) < 1e-4f);
    // As the engine places it (the start heading's frame): steering by the turn equals moving along the heading now.
    const auto world = [](float h, float lx, float ly) {
        return std::pair{lx * std::cos(h) + ly * std::sin(h), -lx * std::sin(h) + ly * std::cos(h)};
    };
    x = 1, y = 2;
    Steer(0.7f, x, y);
    const auto steered = world(0.3f, x, y), direct = world(1.0f, 1, 2);
    assert(std::fabs(steered.first - direct.first) < 1e-4f && std::fabs(steered.second - direct.second) < 1e-4f);
    // The steer from what the engine moved: engine heading 1.10, body 4.89 (log 10-31), a forward clip unsteered
    // moves at 1.10, so the steer is 3.79; once steered, the same measurement keeps it.
    const float engine = 1.10f, body = 4.89f;
    const float steer = SteerFor(body, engine, 0.0f);
    assert(near(steer, body - engine));
    assert(near(SteerFor(body, engine + steer, steer), steer));
    // A sideways clip measures the same engine heading.
    assert(near(SteerFor(body, engine + pi / 2, pi / 2), body - engine));
    // Measured only when both move at speeds that agree.
    assert(Comparable(234, 238) && !Comparable(234, 120) && !Comparable(10, 10) && !Comparable(100, 0));
    std::cout << "TdmTurn ok\n";
    return 0;
}
