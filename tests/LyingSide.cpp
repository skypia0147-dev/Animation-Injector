#include "runtime/LyingSide.h"
#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace AnimationInjector::LyingSide;
int main() {
    unsigned checks = 0;
    auto check = [&](bool condition) {
        ++checks;
        if (!condition) {
            std::cerr << "Failed check " << checks << '\n';
            std::exit(1);
        }
    };
    // Standing upright facing +Y: the chest faces +Y.
    const Vec com{0, 0, 100}, neck{0, 0, 150}, leftArm{-20, 0, 140};
    const auto chest = Chest(com, neck, leftArm);
    check(chest.y > .99f && std::abs(chest.z) < 1e-4f);
    // Fallen backward from facing +Y: head toward -Y, face up, the left arm still toward -X: chest +Z.
    check(OnBack({0, 0, 10}, {0, -50, 10}, {-20, -40, 10}));
    // Head toward +Y and face up puts the left arm toward +X; with it toward -X the face is down.
    check(OnBack({0, 0, 10}, {0, 50, 10}, {20, 40, 10}));
    check(!OnBack({0, 0, 10}, {0, 50, 10}, {-20, 40, 10}));
    // A pose that spans no body counts as the back.
    check(OnBack(com, com, com));
    std::cout << checks << " lying side checks passed\n";
}
