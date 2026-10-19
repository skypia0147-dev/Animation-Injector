#include "ui/QteConverge.h"
#include "ui/QteFace.h"
#include <cmath>
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
    auto near = [](double a, double b, double e = 1e-6) { return std::abs(a - b) <= e; };
    QteStyle s;
    s.design = QteDesign::Converge;
    check(QteFace::ModeOf(s) == QteFace::Mode::Converge);
    // The lines meet in the middle at the approach time; both run from the farther screen edge.
    const auto timing = QteConverge::TimingOf(s);
    check(near(timing.total, 2.4) && near(timing.approach, QteClock::target * 2.4, 1e-5));
    check(near(QteConverge::Reach(1920, 960, 1), 960) && near(QteConverge::Reach(1920, 600, 2), 660));
    QteClock c;
    c.elapsed = static_cast<float>(timing.approach);
    check(near(QteConverge::Head(s, c), 1, 1e-5));
    // Darkening comes in over .3 s and the whole look stays full while running.
    c.elapsed = .15f;
    check(near(QteConverge::Focus(s, c), .375, 1e-5) && QteConverge::Ui(s, c) == 1);
    // The converging lines fail once the success window is over; the circle waits for the end of its run.
    QteStyle circle;
    QteClock run, wait;
    const float late = (QteClock::target + s.successWindow) * 2.4f + .01f;
    run.Advance(late, s);
    wait.Advance(late, circle);
    check(run.outcome == QteOutcome::Failure && wait.outcome == QteOutcome::None);
    // A press inside the window succeeds as before.
    QteClock press;
    press.Advance(static_cast<float>(timing.approach), s);
    press.Press(s);
    check(press.outcome == QteOutcome::Perfect);
    // The result's look: gone by the end of its time on screen, the key dips once on a success.
    s.feedback = 1.2f;
    press.resultElapsed = .5f;
    check(QteConverge::Ui(s, press) == 1);
    press.resultElapsed = 1.2f;
    check(near(QteConverge::Ui(s, press), 0));
    press.resultElapsed = .09f;
    check(near(QteConverge::KeyScale(s, press), .88, 1e-3) && QteFace::LookOf(s, press).scale < 1);
    press.resultElapsed = .2f;
    check(near(QteConverge::KeyScale(s, press), 1));
    check(QteConverge::KeyColor(QteOutcome::Perfect) == QteConverge::KeyColor(QteOutcome::Success));
    check(QteConverge::KeyColor(QteOutcome::Failure) != QteConverge::KeyColor(QteOutcome::None));
    // Flash on a success only, fading fast.
    check(near(QteConverge::Flash(press), .1 * std::exp(-2.0), 1e-6));
    run.resultElapsed = 0;
    check(QteConverge::Flash(run) == 0);
    // A miss drains colour in by .25 s (peak .9), holds, and gives it back by 2.1 s.
    check(QteConverge::Desat(0) == 0 && near(QteConverge::Desat(.25), .9) && near(QteConverge::Desat(1.4), .9));
    check(QteConverge::Desat(1.8) > 0 && QteConverge::Desat(1.8) < .9 && QteConverge::Desat(2.1) == 0);
    check(QteConverge::Shake(QteOutcome::Failure) > QteConverge::Shake(QteOutcome::Success));
    check(QteConverge::Shake(QteOutcome::Perfect) > QteConverge::Shake(QteOutcome::Success));
    // Particles are the same every frame of a session (no state kept between frames) and differ between sessions.
    const auto a = QteConverge::SparkOf(7, 3, QteOutcome::Perfect), b = QteConverge::SparkOf(7, 3, QteOutcome::Perfect);
    check(a.vx == b.vx && a.vy == b.vy && a.life == b.life);
    check(QteConverge::SparkOf(8, 3, QteOutcome::Perfect).vx != a.vx);
    check(QteConverge::SparkCount(QteOutcome::Perfect) == 64 && QteConverge::SparkCount(QteOutcome::Success) == 26);
    bool ashInRange = true;
    for (int i = 0; i < QteConverge::kAshCount; ++i) {
        const auto ash = QteConverge::AshOf(7, i);
        ashInRange &= ash.back >= 0 && ash.back < 1 && ash.vy < 0 && ash.life >= 1 && ash.life <= 2.3;
        ashInRange &= ash.side == (i % 2 ? 1 : -1) && std::abs(ash.y) <= 1.5;
    }
    check(ashInRange);
    // The closed-form flight matches small fixed steps of the mockup's drag and gravity.
    double x = 0, y = 0, vx = 900, vy = -40;
    const double k = 3.2, g = 60, dt = 1e-4;
    for (int i = 0; i < 5000; ++i) {
        const double e = std::exp(-k * dt);
        vx *= e;
        vy = vy * e + g * dt;
        x += vx * dt;
        y += vy * dt;
    }
    const auto fly = QteConverge::Fly(0, 0, 900, -40, k, g, .5);
    check(near(fly.x, x, .5) && near(fly.y, y, .5) && near(fly.vx, vx, .5));
    // The editor's preview (qteConverge.ts) draws the same particles: the same numbers in its test.
    check(near(QteConverge::Hash(7, 3), 0.7062613831367344, 1e-12) &&
          near(QteConverge::Hash(123456, 240), 0.12778993975371122, 1e-12));
    check(near(a.vx, -641.6430348996073, 1e-9) && near(a.life, 0.5125156966736539, 1e-12));
    check(near(QteConverge::AshOf(7, 5).back, 0.5305424235914228, 1e-12));
    std::cout << checks << " checks passed\n";
}
