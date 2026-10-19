#include "ui/QteLayout.h"
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <limits>
#include <vector>
using namespace AnimationInjector;
unsigned checks = 0;
void Check(bool value) {
    ++checks;
    if (!value) throw std::runtime_error("QTE layout assertion failed");
}
bool Near(double a, double b) { return std::abs(a - b) < 1e-8; }
// One QTE of every face and type; the editor's tools/qte-layout-check.mjs builds the same list.
struct Case {
    QteStyle style;
    double keyWidth;
};
std::vector<Case> Cases() {
    auto style = [](QteType type, QteDesign design = QteDesign::Circle, std::uint32_t presses = 10,
                     std::uint32_t sequence = 0) {
        QteStyle s;
        s.type = type;
        s.design = design;
        s.pressCount = presses;
        s.sequenceCount = sequence;
        return s;
    };
    // A mouse button is a keycap named LMB (43 wide) since 0.12.258.
    return {{style(QteType::SplitArc), 30}, {style(QteType::SplitArc), 187},
        {style(QteType::SplitArc, QteDesign::Converge), 30}, {style(QteType::SplitArc, QteDesign::Converge), 43},
        {style(QteType::SplitArc, QteDesign::Converge), 250}, {style(QteType::RapidPress, QteDesign::Circle, 1), 30},
        {style(QteType::RapidPress, QteDesign::Circle, 1), 61}, {style(QteType::RapidPress, QteDesign::Circle, 1), 43},
        {style(QteType::TimingBar), 30}, {style(QteType::RotatingNeedle), 30}, {style(QteType::HoldRelease), 30},
        {style(QteType::RapidPress), 30}, {style(QteType::KeySequence, QteDesign::Circle, 10, 3), 30},
        {style(QteType::KeySequence, QteDesign::Circle, 10, 8), 30}};
}
int main(int argc, char** argv) {
    try {
        using namespace QteLayout;
        const bool samples = argc > 1 && std::string_view(argv[1]) == "--samples";
        std::cout << std::setprecision(17);
        const auto cases = Cases();
        for (unsigned c = 0; c < cases.size(); ++c) {
            const auto b = BoundsOf(cases[c].style, cases[c].keyWidth);
            Check(b.left < 0 && b.top < 0 && b.right > 0 && b.bottom > 0);
            for (auto size : {std::pair{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}, {7680, 4320},
                     {1920, 1200}, {1600, 1200}, {3440, 1440}, {5120, 1440}, {1080, 1920}, {64, 4096}, {4096, 64}})
                for (unsigned a = 0; a < qteAnchorNames.size(); ++a)
                    for (double scale : {.25, 1., 4.})
                        for (double offset : {0., .15, 1.}) {
                            const auto anchor = static_cast<QteAnchor>(a);
                            const auto p = Calculate(size.first, size.second, scale, offset, offset, anchor, b);
                            Check(p.unit > 0 && p.x + b.left * p.unit >= p.padding - 1e-8 &&
                                  p.x + b.right * p.unit <= size.first - p.padding + 1e-8);
                            Check(p.y + b.top * p.unit >= p.padding - 1e-8 &&
                                  p.y + b.bottom * p.unit <= size.second - p.padding + 1e-8);
                            const auto doubled =
                                Calculate(size.first * 2, size.second * 2, scale, offset, offset, anchor, b);
                            Check(
                                Near(doubled.x, p.x * 2) && Near(doubled.y, p.y * 2) && Near(doubled.unit, p.unit * 2));
                            const auto movie = ToMovie(p, size.first, size.second, 7, 13, 1280, 720);
                            Check(Near((movie.x - 7) * size.first / 1280, p.x) &&
                                  Near((movie.y - 13) * size.second / 720, p.y));
                            Check(Near(movie.scaleX / 100 * size.first / 1280, p.unit) &&
                                  Near(movie.scaleY / 100 * size.second / 720, p.unit));
                            if (samples)
                                std::cout << size.first << ' ' << size.second << ' ' << scale << ' ' << offset << ' '
                                          << a << ' ' << c << ' ' << p.x << ' ' << p.y << ' ' << p.unit << ' '
                                          << p.padding << ' ' << p.fitted << '\n';
                        }
            // A corner holds the drawing's near edge: the gap to it is the same at every scale that fits.
            for (auto a : {QteAnchor::TopLeft, QteAnchor::TopRight, QteAnchor::BottomLeft, QteAnchor::BottomRight}) {
                const bool east = a == QteAnchor::TopRight || a == QteAnchor::BottomRight;
                const bool south = a == QteAnchor::BottomLeft || a == QteAnchor::BottomRight;
                for (double scale : {.5, 1., 1.5}) {
                    const auto p = Calculate(1920, 1080, scale, .05, .05, a, b);
                    const double gapX = east ? 1920 - (p.x + b.right * p.unit) : p.x + b.left * p.unit;
                    const double gapY = south ? 1080 - (p.y + b.bottom * p.unit) : p.y + b.top * p.unit;
                    Check(Near(gapX, p.padding + 54) && Near(gapY, p.padding + 54));
                }
            }
            // The middle and a screen position hold the drawing's middle at every scale away from the edges.
            for (double scale : {.5, 1., 1.5}) {
                const auto m = Calculate(1920, 1080, scale, .1, -.1, QteAnchor::Center, b);
                Check(Near(m.x, 1068) && Near(m.y, 432));
                const auto s = Calculate(1920, 1080, scale, .5, .5, QteAnchor::Screen, b);
                Check(Near(s.x, 960) && Near(s.y, 540));
            }
        }
        // The circle before: a corner circle grew away from its corner (54, 94 and 174 pixels at .5, 1 and 2).
        const auto circle = BoundsOf(cases[0].style, 30);
        for (double scale : {.5, 1., 2.}) {
            const auto p = Calculate(1920, 1080, scale, 0, 0, QteAnchor::TopLeft, circle);
            Check(Near(p.x - 104 * p.unit, p.padding) && Near(p.y - 104 * p.unit, p.padding));
        }
        // A key wider than the circle widens it (a 250 cap, 1 for its outline).
        Check(BoundsOf(cases[0].style, 187).right == 104 && BoundsOf(cases[0].style, 250).right == 126);
        auto c = Calculate(1920, 1080, 1, 0, 0, QteAnchor::Center, circle);
        Check(c.x == 960 && c.y == 540 && c.unit == 1);
        auto u = Calculate(3840, 1080, 1, 0, 0, QteAnchor::Center, circle);
        Check(u.x == 1920 && u.y == c.y && u.unit == c.unit);
        for (auto a : {QteAnchor::TopLeft, QteAnchor::TopRight, QteAnchor::BottomLeft, QteAnchor::BottomRight}) {
            auto p = Calculate(1920, 1080, 1, .1, .1, a, circle), wide = Calculate(3840, 1080, 1, .1, .1, a, circle);
            Check(Near(p.y, wide.y) && Near(p.unit, wide.unit));
            Check(Near(wide.x - p.x, (a == QteAnchor::TopRight || a == QteAnchor::BottomRight) ? 1920 : 0));
        }
        const auto neg = Calculate(1920, 1080, 1, -.1, -.1, QteAnchor::Center, circle);
        Check(Near(neg.x, 852) && Near(neg.y, 432));
        Check(Calculate(0, 1080, 1, 0, 0, QteAnchor::Screen, circle).unit == 0);
        Check(Calculate(std::numeric_limits<double>::quiet_NaN(), 1080, 1, 0, 0, QteAnchor::Screen, circle).unit == 0);
        Check(Calculate(1920, 1080, 1, 0, 0, QteAnchor::Screen, Bounds{}).unit == 0);
        Check(Calculate(1080, 1920, 4, 0, 0, QteAnchor::Center, circle).fitted);
        if (!samples) std::cout << "QTE layout: " << checks << " checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what();
        return 1;
    }
}
