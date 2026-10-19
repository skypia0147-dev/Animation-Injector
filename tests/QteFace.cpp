#include "ui/QteFace.h"
#include <cstdlib>
#include <iostream>

using namespace AnimationInjector;
int main() {
    unsigned checks = 0;
    auto check = [&](bool condition) {
        ++checks;
        if (!condition) {
            std::cerr << "Failed check " << checks << '\n';
            std::exit(1);
        }
    };
    QteStyle circle, single, rapid, bar;
    circle.type = QteType::SplitArc;
    single.type = rapid.type = QteType::RapidPress;
    single.pressCount = 1;
    rapid.pressCount = 2;
    bar.type = QteType::TimingBar;
    // The circle and a one-press QTE show the key themselves; every other kind keeps its hint line.
    check(QteFace::ModeOf(circle) == QteFace::Mode::Circle);
    check(QteFace::ModeOf(single) == QteFace::Mode::Single);
    check(QteFace::ModeOf(rapid) == QteFace::Mode::Hint && QteFace::ModeOf(bar) == QteFace::Mode::Hint);
    // A mouse button is a keycap named LMB, RMB or MMB since 0.12.258 (no mouse drawing): see QteLayout.cpp.

    // Waiting: the key as it is.
    QteClock c;
    auto look = QteFace::LookOf(single, c);
    check(look.scale == 1 && look.alpha == 1 && look.flash == 0);
    // Pressed: down, back up past its size, lit from inside, gone by kPressSeconds.
    c.Press(single);
    check(c.outcome == QteOutcome::Perfect || c.outcome == QteOutcome::Success);
    c.resultElapsed = .03f;
    look = QteFace::LookOf(single, c);
    check(look.scale < 1 && look.alpha == 1 && look.flash > .4);
    c.resultElapsed = .12f;
    look = QteFace::LookOf(single, c);
    check(look.scale > .86 && look.scale < 1.06 && look.flash > 0);
    c.resultElapsed = .3f;
    look = QteFace::LookOf(single, c);
    check(look.scale > 1 && look.alpha > 0 && look.alpha < 1 && look.flash == 0);
    c.resultElapsed = static_cast<float>(QteFace::kPressSeconds) + .01f;
    check(QteFace::LookOf(single, c).alpha == 0);
    // Not pressed in time: gone at once, no result shown.
    QteClock late;
    late.Advance(10, single);
    check(late.outcome == QteOutcome::Failure && QteFace::LookOf(single, late).alpha == 0);
    // Reduced motion: no bounce, a short fade.
    single.reducedMotion = true;
    c.resultElapsed = .1f;
    look = QteFace::LookOf(single, c);
    check(look.scale == 1 && look.alpha > 0 && look.alpha < 1 && look.flash == 0);

    // The circle's key fades with its result ring.
    QteClock judged;
    judged.Press(circle);
    check(judged.outcome != QteOutcome::None);
    judged.resultElapsed = 0;
    check(QteFace::LookOf(circle, judged).alpha == 1);
    judged.resultElapsed = circle.feedback;
    check(QteFace::LookOf(circle, judged).alpha == 0);
    std::cout << checks << " checks passed\n";
}
