#pragma once
#include <algorithm>
#include <cmath>
#include "runtime/QteSpec.h"
#include "ui/QteFace.h"

namespace AnimationInjector::QteLayout {
inline constexpr double referenceHeight = 1080, paddingRatio = .0125;
// What a QTE draws, in drawing units about its middle (0.12.257). The anchors and the screen's edges hold this box,
// not one sized for every type: a box larger than the drawing left an empty margin that grew with the scale, so a
// corner QTE moved away from its corner as it grew.
struct Bounds {
    double left = 0, top = 0, right = 0, bottom = 0;
};
// The key a QTE draws (QteMenu.cpp KeyCap): a cap this wide and 30 high.
inline Bounds BoundsOf(const QteStyle& s, double keyWidth) {
    const double half = std::max(30.0, keyWidth) / 2 + 1;
    switch (QteFace::ModeOf(s)) {
    case QteFace::Mode::Circle: {
        // The shrinking circle starts at 92 and the result ring ends at 101 (3 wide).
        const double r = std::max(104.0, half);
        return {-r, -r, r, r};
    }
    case QteFace::Mode::Single: {
        // The key alone, 1.4 times larger, and its press springs up to 1.06.
        const double k = QteFace::kSingleScale * 1.06, w = half, h = 16;
        return {-w * k, -h * k, w * k, h * k};
    }
    case QteFace::Mode::Converge: {
        // The lines cross the whole screen; what stays is the diamond, its result glow and the key's name (size 30).
        const double w = std::max(112.0, std::max(30.0, keyWidth));
        return {-w, -112, w, 112};
    }
    default: {
        // The widget, the key's name at 100 and the hint at 130 (24 high) below it.
        const double w = std::max(122.0, s.type == QteType::KeySequence ? s.sequenceCount * 22.0 + 2 : 0.0);
        const double top = s.type == QteType::RotatingNeedle || s.type == QteType::HoldRelease ? -72
                           : s.type == QteType::KeySequence                                    ? -22
                           : s.type == QteType::TimingBar                                      ? -19
                                                                                               : -11;
        return {-w, top, w, 156};
    }
    }
}
struct Placement {
    double x = 0, y = 0, unit = 0, padding = 0;
    bool fitted = false;
};
inline Placement Calculate(
    double width, double height, double scale, double x, double y, QteAnchor anchor, const Bounds& b) {
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) return {};
    if (!(b.right > b.left && b.bottom > b.top)) return {};
    scale = std::isfinite(scale) ? std::clamp(scale, .25, 4.0) : 1;
    const bool screen = anchor == QteAnchor::Screen;
    x = std::isfinite(x) ? std::clamp(x, screen ? 0.0 : -1.0, 1.0) : (screen ? .5 : 0);
    y = std::isfinite(y) ? std::clamp(y, screen ? 0.0 : -1.0, 1.0) : (screen ? .5 : 0);
    const double padding = std::min(width, height) * paddingRatio;
    const double requested = height / referenceHeight * scale;
    const double unit =
        std::min({requested, (width - 2 * padding) / (b.right - b.left), (height - 2 * padding) / (b.bottom - b.top)});
    double px = width * x, py = height * y;
    if (!screen) {
        // A corner holds the drawing's near edge, so a larger scale grows away from the corner.
        const bool center = anchor == QteAnchor::Center;
        const bool east = anchor == QteAnchor::TopRight || anchor == QteAnchor::BottomRight;
        const bool south = anchor == QteAnchor::BottomLeft || anchor == QteAnchor::BottomRight;
        px = (center    ? width / 2
                 : east ? width - padding - b.right * unit
                        : padding - b.left * unit) +
             (east ? -x : x) * height;
        py = (center     ? height / 2
                 : south ? height - padding - b.bottom * unit
                         : padding - b.top * unit) +
             (south ? -y : y) * height;
    }
    return {std::clamp(px, padding - b.left * unit, width - padding - b.right * unit),
        std::clamp(py, padding - b.top * unit, height - padding - b.bottom * unit), unit, padding, unit < requested};
}
struct MoviePlacement {
    double x = 0, y = 0, scaleX = 0, scaleY = 0;
};
inline MoviePlacement ToMovie(const Placement& p, double viewportW, double viewportH, double frameLeft, double frameTop,
    double frameW, double frameH) {
    if (p.unit <= 0 || viewportW <= 0 || viewportH <= 0 || !std::isfinite(frameW) || !std::isfinite(frameH) ||
        frameW <= 0 || frameH <= 0)
        return {};
    const double sx = frameW / viewportW, sy = frameH / viewportH;
    return {frameLeft + p.x * sx, frameTop + p.y * sy, 100 * p.unit * sx, 100 * p.unit * sy};
}
}
