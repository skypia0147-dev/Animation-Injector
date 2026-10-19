#pragma once
#include <array>
#include <algorithm>
#include <string>
#include <cstdint>
#include <cmath>
#include <string_view>

namespace AnimationInjector {
enum class QteAnchor { Screen, Center, TopLeft, TopRight, BottomLeft, BottomRight };
inline constexpr std::array<std::string_view, 6> qteAnchorNames{
    "screen", "center", "topLeft", "topRight", "bottomLeft", "bottomRight"};
inline QteAnchor ParseQteAnchor(std::string_view name) {
    for (std::size_t i = 0; i < qteAnchorNames.size(); ++i)
        if (qteAnchorNames[i] == name) return static_cast<QteAnchor>(i);
    return QteAnchor::Screen;
}
enum class QteOutcome { None, Success, Failure, Perfect };
enum class QteType { SplitArc, TimingBar, RotatingNeedle, HoldRelease, RapidPress, KeySequence };
inline constexpr std::array<std::string_view, 6> qteTypeNames{
    "splitArc", "timingBar", "rotatingNeedle", "holdRelease", "rapidPress", "keySequence"};
inline QteType ParseQteType(std::string_view name) {
    for (std::size_t i = 0; i < qteTypeNames.size(); ++i)
        if (qteTypeNames[i] == name) return static_cast<QteType>(i);
    return QteType::SplitArc;
}
// How a splitArc QTE looks (0.12.246): the shrinking circle, or two lines converging from the screen's edges to the
// middle (the user's QTE_Mockups converging-lines design).
enum class QteDesign { Circle, Converge };
inline constexpr std::array<std::string_view, 2> qteDesignNames{"circle", "converge"};
struct QteKey {
    std::uint32_t key = 18, gamepad = 0;
};
struct QteResultSpec {
    std::string event;
    std::string recipient = "self";
    std::string soundPlugin, soundForm;
    // An event's QTE (0.12.249): the action this result starts (an action's QTE turns its `to` into a link instead).
    // In a pair (0.12.264) `to` is the player's and `partnerTo` the pair partner's.
    std::string to, partnerTo;
};
struct QteStyle {
    QteAnchor anchor = QteAnchor::Screen;
    QteType type = QteType::SplitArc;
    std::uint32_t pressCount = 10, sequenceCount = 0;
    std::array<QteKey, 8> sequence{};
    float perfectBefore = .5f;
    std::uint32_t key = 0x12, gamepad = 0, alternateKey = 0;
    float speed = 1, successWindow = .085f, perfectWindow = .026f;
    float scale = 1, x = .5f, y = .5f, delay = 0, feedback = 1.35f;
    bool reducedMotion = false;
    // Nothing on screen (0.12.266): the input and its results only, for a press or a mash played as a moment of the
    // animation. Its result ends at once (no result to show), and the converging lines' screen effects stay off.
    bool hidden = false;
    QteDesign design = QteDesign::Circle;
};
struct QteSpec : QteStyle {
    std::string id, event, stopEvent, control;
    bool replaceActive = false;
    float at = -1, fallback = -1;
    std::string inputOwner = "player", eventSource = "self";
    std::array<QteResultSpec, 3> results;
    // QTEs on different channels run, draw and take input independently (0..3). One channel
    // still holds one challenge at a time: a later one on a busy channel is skipped as before.
    std::uint8_t channel = 0;
};
// Pure timing model shared by the runtime and deterministic regression tests.
struct QteClock {
    static constexpr float target = 43.0f / 69.0f;
    float elapsed = 0, resultElapsed = 0;
    QteOutcome outcome = QteOutcome::None;
    bool sent = false;
    bool holding = false;
    float heldElapsed = 0;
    std::uint32_t completed = 0;
    float Progress(const QteStyle& s) const { return elapsed * s.speed / 2.4f; }
    float Gauge(const QteStyle& s) const {
        if (s.type == QteType::HoldRelease) return heldElapsed * s.speed / 2.4f;
        if (s.type == QteType::RapidPress) return static_cast<float>(completed) / s.pressCount;
        if (s.type == QteType::KeySequence) return static_cast<float>(completed) / std::max(1u, s.sequenceCount);
        return Progress(s);
    }
    void Resolve(QteOutcome value) {
        if (outcome == QteOutcome::None) {
            outcome = value;
            resultElapsed = 0;
        }
    }
    void Judge(float p, const QteStyle& s) {
        const float error = std::fabs(p - target);
        Resolve(p >= 1                    ? QteOutcome::Failure
                : error < s.perfectWindow ? QteOutcome::Perfect
                : error < s.successWindow ? QteOutcome::Success
                                          : QteOutcome::Failure);
    }
    void Press(const QteStyle& s, bool expected = true) {
        if (outcome != QteOutcome::None) return;
        if (Progress(s) >= 1) {
            Resolve(QteOutcome::Failure);
            return;
        }
        if (s.type == QteType::HoldRelease) {
            holding = true;
            return;
        }
        if (s.type == QteType::RapidPress || s.type == QteType::KeySequence) {
            if (!expected) {
                Resolve(QteOutcome::Failure);
                return;
            }
            if (++completed >= (s.type == QteType::RapidPress ? s.pressCount : s.sequenceCount))
                Resolve(
                    s.perfectBefore > 0 && Progress(s) <= s.perfectBefore ? QteOutcome::Perfect : QteOutcome::Success);
            return;
        }
        Judge(Progress(s), s);
    }
    void Release(const QteStyle& s) {
        if (s.type != QteType::HoldRelease || !holding) return;
        holding = false;
        if (Progress(s) >= 1)
            Resolve(QteOutcome::Failure);
        else
            Judge(Gauge(s), s);
    }
    void Advance(float delta, const QteStyle& s) {
        if (!std::isfinite(delta) || delta < 0) return;
        if (outcome != QteOutcome::None)
            resultElapsed += delta;
        else {
            elapsed += delta;
            if (holding) heldElapsed += delta;
            // Converging lines have crossed once the success window is over: nothing later can succeed.
            if (Progress(s) >= 1 || (s.design == QteDesign::Converge && Progress(s) > target + s.successWindow))
                Resolve(QteOutcome::Failure);
        }
    }
};
}
