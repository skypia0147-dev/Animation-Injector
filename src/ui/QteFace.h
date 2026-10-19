#pragma once
#include "runtime/QteSpec.h"
#include "ui/QteConverge.h"
#include <algorithm>

// How a QTE shows its input key (0.12.245, from the user's QTE shape mockup). Pure, so the menu (ui/QteMenu.cpp), the
// editor's preview (QtePreview.tsx) and the tests agree.
namespace AnimationInjector::QteFace {
// Hint: the key's name under the widget and a hint line (timing bar, needle, hold, rapid press of 2+, sequence).
// Circle: the shrinking circle draws the key in its middle and no text.
// Single: a rapid press of one press draws the key alone: no bar, no time line, no text, no result.
// Converge: the converging lines (0.12.246) show the key's name bare in the middle.
// A mouse button is named like any key (LMB, RMB, MMB) since 0.12.258; it was drawn as a mouse before.
enum class Mode { Hint, Circle, Single, Converge };
inline Mode ModeOf(const QteStyle& s) {
    if (s.type == QteType::SplitArc) return s.design == QteDesign::Converge ? Mode::Converge : Mode::Circle;
    if (s.type == QteType::RapidPress && s.pressCount == 1) return Mode::Single;
    return Mode::Hint;
}
// The one-press QTE's key is this much larger than the key inside the circle.
inline constexpr double kSingleScale = 1.4;
// How long the press plays on a one-press QTE (down, back up, lit from inside, gone).
inline constexpr double kPressSeconds = .38;
struct Look {
    double scale = 1, alpha = 1, flash = 0;
};
// The key this frame. In the circle it fades with the result ring. A one-press QTE plays the press when pressed
// (success or perfect alike) and is gone at once when its time runs out; it never shows a result.
inline Look LookOf(const QteStyle& s, const QteClock& c) {
    if (c.outcome == QteOutcome::None) return {};
    if (ModeOf(s) == Mode::Converge) return {QteConverge::KeyScale(s, c), QteConverge::Ui(s, c), 0};
    const double t = c.resultElapsed;
    if (ModeOf(s) == Mode::Single) {
        if (c.outcome == QteOutcome::Failure) return {1, 0, 0};
        if (s.reducedMotion) return {1, std::clamp(1 - t / .25, 0.0, 1.0), 0};
        const double scale = t < .06 ? 1 - .14 * (t / .06) : t < .16 ? .86 + .2 * ((t - .06) / .1) : 1.06;
        const double alpha = t < .16 ? 1 : std::clamp(1 - (t - .16) / (kPressSeconds - .16), 0.0, 1.0);
        return {scale, alpha, .55 * std::clamp(1 - t / .2, 0.0, 1.0)};
    }
    const double k =
        s.reducedMotion ? .28 : std::clamp(t / std::max(.001, static_cast<double>(s.feedback) * .85), 0.0, 1.0);
    return {1, 1 - k, 0};
}
}
