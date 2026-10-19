#pragma once
#include "runtime/QteSpec.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

// The converging-lines look of a splitArc QTE (0.12.246, from the user's QTE_Mockups page): two hairlines run from
// the screen's edges and meet in the middle at the target time. Pure, so the menu (ui/QteMenu.cpp), the screen
// effects (runtime/QteScreenFx.cpp), the editor's preview and the tests agree. Sizes are in pixels at 1080 lines.
namespace AnimationInjector::QteConverge {
inline double Clamp01(double v) { return std::clamp(v, 0.0, 1.0); }
inline double EaseOut(double t) { return 1 - std::pow(1 - Clamp01(t), 3); }
inline double EaseOut5(double t) { return 1 - std::pow(1 - Clamp01(t), 5); }
inline double EaseIn(double t) {
    t = Clamp01(t);
    return t * t;
}
// Seconds: the whole run (2.4 s at speed 1), the approach (until the lines meet), the windows either side.
struct Timing {
    double total, approach, perfect, success;
};
inline Timing TimingOf(const QteStyle& s) {
    const double total = 2.4 / std::max(.1f, s.speed);
    return {total, QteClock::target * total, s.perfectWindow * total, s.successWindow * total};
}
// How far the lines have come: 0 at the edges, 1 met in the middle (a little past it until the window closes).
inline double Head(const QteStyle& s, const QteClock& c) { return c.Progress(s) / QteClock::target; }
// The mockup adds two blurred copies of its lights (bloom, .85 and .7). A broad soft light passes through the blur
// almost whole, so it shows about this much brighter there; the game has no blur and draws such lights this much
// brighter instead (thin lines keep their own alpha).
inline constexpr double kGlowGain = 2.2;
// 1 for a perfect press, .62 for a plain success (the mockup's q).
inline double Quality(QteOutcome o) { return o == QteOutcome::Perfect ? 1 : .62; }
// The whole look's opacity: full while running, gone over the last .45 s of the result's time on screen.
inline double Ui(const QteStyle& s, const QteClock& c) {
    if (c.outcome == QteOutcome::None) return 1;
    const double shown = std::max(s.feedback, s.delay);
    return 1 - Clamp01((c.resultElapsed - (shown - .45)) / .45);
}
// Darkening around the band while the QTE is up (in over the first .3 s), and the brief white lift on a success.
inline double Focus(const QteStyle& s, const QteClock& c) { return .75 * Ui(s, c) * Clamp01(c.elapsed / .3); }
inline double Flash(const QteClock& c) {
    if (c.outcome == QteOutcome::None || c.outcome == QteOutcome::Failure) return 0;
    return .1 * Quality(c.outcome) * std::exp(-c.resultElapsed * 10);
}
// A miss drains the world's colour: in by .25 s, back by 2.1 s (peak .9).
inline constexpr double kDesatSeconds = 2.1;
inline double Desat(double t) {
    if (t < 0 || t >= kDesatSeconds) return 0;
    return EaseOut(t / .25) * (1 - EaseOut((t - 1.5) / .6)) * .9;
}
// How far each line starts from the middle, in drawing units: the farther screen edge, so both heads stay the same
// distance from the middle and meet together wherever the QTE sits (960 at 1080 lines, centred).
inline double Reach(double width, double x, double unit) { return unit > 0 ? std::max(x, width - x) / unit : 0; }
// The key's name in the middle: warm white while running, gold when pressed in time, ash after a miss. A success
// dips it once (12% smaller at .09 s, back by .18 s).
inline unsigned KeyColor(QteOutcome o) {
    return o == QteOutcome::None ? 0xF2E8D5 : o == QteOutcome::Failure ? 0xBEA096 : 0xFFD696;
}
inline double KeyScale(const QteStyle& s, const QteClock& c) {
    if (s.reducedMotion || c.outcome == QteOutcome::None || c.outcome == QteOutcome::Failure) return 1;
    return 1 - .12 * std::sin(Clamp01(c.resultElapsed / .18) * 3.14159265358979);
}
// Camera shake for a result (strength in the game's 0..1 scale): harder for a perfect press, firm for a miss.
inline double Shake(QteOutcome o) { return o == QteOutcome::Failure ? .3 : .22 * Quality(o); }
// A stable random number in [0, 1) for particle `i` of a session.
inline double Hash(std::uint32_t a, std::uint32_t b) {
    std::uint32_t h = a * 374761393u ^ b * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<double>(h ^ (h >> 16)) / 4294967296.0;
}
// A particle under drag `k` (1/s) and gravity `g` (px/s^2, down positive) after `t` seconds: closed form, so the
// menu keeps no particle state between frames.
struct Point {
    double x, y, vx, vy;
};
inline Point Fly(double x, double y, double vx, double vy, double k, double g, double t) {
    const double e = std::exp(-k * t), f = (1 - e) / k;
    return {x + vx * f, y + vy * f + g / k * (t - f), vx * e, vy * e + g / k * (1 - e)};
}
// Sparks of a success: count, then each one's start velocity and life (perfect: 64 fast, success: 26 slower).
inline int SparkCount(QteOutcome o) { return o == QteOutcome::Perfect ? 64 : 26; }
struct Spark {
    double vx, vy, life;
};
inline Spark SparkOf(std::uint64_t session, int i, QteOutcome o) {
    const auto s = static_cast<std::uint32_t>(session);
    const double side = Hash(s, i * 4) < .5 ? -1 : 1;
    const double speed = (250 + Hash(s, i * 4 + 1) * 1300) * (o == QteOutcome::Perfect ? 1 : .6);
    return {side * speed, (Hash(s, i * 4 + 2) - .5) * 170, .35 + Hash(s, i * 4 + 3) * .7};
}
// Ash of a miss: 240 flecks along both tails (side 1 the left one), lit in turn from the heads back, drifting up.
inline constexpr int kAshCount = 240;
struct Ash {
    double side, back, y, vx, vy, life, wobble;
};
inline Ash AshOf(std::uint64_t session, int i) {
    const auto s = static_cast<std::uint32_t>(session) ^ 0x9e3779b9u;
    const double back = std::pow(Hash(s, i * 7), 1.8);  // 0 at the head .. 1 at the far end of the tail
    return {i % 2 ? 1.0 : -1.0, back, (Hash(s, i * 7 + 5) - .5) * 3, (Hash(s, i * 7 + 1) - .5) * 40,
        -(8 + Hash(s, i * 7 + 2) * 50), 1 + Hash(s, i * 7 + 3) * 1.3, Hash(s, i * 7 + 4) * 6};
}
}
