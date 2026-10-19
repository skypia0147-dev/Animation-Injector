#include "PCH.h"
#include "ui/QteMenu.h"
#include "ui/QteLayout.h"
#include "ui/QteFace.h"
#include "ui/QteConverge.h"
#include "runtime/Qte.h"
#include "runtime/QteServices.h"
#include <numbers>
#include <cstdio>
#include <span>

namespace AnimationInjector::QteMenu {
namespace {
constexpr std::string_view name = "AnimationInjectorQTE";
bool registered = false;
constexpr double tau = 2 * std::numbers::pi;
void Arc(RE::GFxValue& clip, double radius, double start, double end, double width, unsigned color, double alpha) {
    const RE::GFxValue style[]{
        RE::GFxValue(width), RE::GFxValue(color), RE::GFxValue(std::clamp(alpha, 0.0, 1.0) * 100)};
    clip.Invoke("lineStyle", nullptr, style, 3);
    const int segments = std::max(2, static_cast<int>(std::ceil((end - start) * 14)));
    for (int i = 0; i <= segments; ++i) {
        const double angle = start + (end - start) * i / segments;
        const RE::GFxValue point[]{RE::GFxValue(radius * std::cos(angle)), RE::GFxValue(radius * std::sin(angle))};
        clip.Invoke(i ? "lineTo" : "moveTo", nullptr, point, 2);
    }
}
void Ring(RE::GFxValue& clip, double radius, double gap, double width, unsigned color, double alpha) {
    for (int i = 0; i < 8; ++i)
        Arc(clip, radius, i * tau / 8 + gap / 2, (i + 1) * tau / 8 - gap / 2, width, color, alpha);
}
void Line(
    RE::GFxValue& clip, double x, double y, double xx, double yy, double width, unsigned color, double alpha = 1) {
    const RE::GFxValue style[]{RE::GFxValue(width), RE::GFxValue(color), RE::GFxValue(alpha * 100)};
    clip.Invoke("lineStyle", nullptr, style, 3);
    const RE::GFxValue from[]{RE::GFxValue(x), RE::GFxValue(y)}, to[]{RE::GFxValue(xx), RE::GFxValue(yy)};
    clip.Invoke("moveTo", nullptr, from, 2);
    clip.Invoke("lineTo", nullptr, to, 2);
}
void Box(RE::GFxValue& clip, double x, double y, double width, double height, unsigned color, double alpha = 1) {
    clip.Invoke("lineStyle", nullptr, nullptr, 0);
    const RE::GFxValue fill[]{RE::GFxValue(color), RE::GFxValue(alpha * 100)};
    clip.Invoke("beginFill", nullptr, fill, 2);
    const double points[][2] = {{x, y}, {x + width, y}, {x + width, y + height}, {x, y + height}, {x, y}};
    for (unsigned i = 0; i < 5; ++i) {
        const RE::GFxValue p[]{RE::GFxValue(points[i][0]), RE::GFxValue(points[i][1])};
        clip.Invoke(i ? "lineTo" : "moveTo", nullptr, p, 2);
    }
    clip.Invoke("endFill", nullptr, nullptr, 0);
}
// A rounded box, outlined, optionally filled with the outline's colour.
void RoundBox(RE::GFxValue& clip, double x, double y, double w, double h, double r, double line, unsigned color,
    double alpha, double fillAlpha) {
    const RE::GFxValue style[]{
        RE::GFxValue(line), RE::GFxValue(color), RE::GFxValue(std::clamp(alpha, 0.0, 1.0) * 100)};
    clip.Invoke("lineStyle", nullptr, style, 3);
    if (fillAlpha > 0) {
        const RE::GFxValue fill[]{RE::GFxValue(color), RE::GFxValue(std::clamp(fillAlpha, 0.0, 1.0) * 100)};
        clip.Invoke("beginFill", nullptr, fill, 2);
    }
    auto to = [&](const char* how, double px, double py) {
        const RE::GFxValue p[]{RE::GFxValue(px), RE::GFxValue(py)};
        clip.Invoke(how, nullptr, p, 2);
    };
    auto curve = [&](double cx, double cy, double px, double py) {
        const RE::GFxValue p[]{RE::GFxValue(cx), RE::GFxValue(cy), RE::GFxValue(px), RE::GFxValue(py)};
        clip.Invoke("curveTo", nullptr, p, 4);
    };
    to("moveTo", x + r, y);
    to("lineTo", x + w - r, y);
    curve(x + w, y, x + w, y + r);
    to("lineTo", x + w, y + h - r);
    curve(x + w, y + h, x + w - r, y + h);
    to("lineTo", x + r, y + h);
    curve(x, y + h, x, y + h - r);
    to("lineTo", x, y + r);
    curve(x, y, x + r, y);
    if (fillAlpha > 0) clip.Invoke("endFill", nullptr, nullptr, 0);
}
// The input key as the circle and the one-press QTE show it (ui/QteFace.h): a keycap whose name is the key label
// text field. A mouse button is a keycap too, named LMB, RMB or MMB (0.12.258). Its size is fixed per session.
struct KeyShape {
    double width = 30;
    bool pad = false;
};
void KeyCap(RE::GFxValue& ring, const KeyShape& shape, double scale, unsigned color, double alpha, double flash) {
    if (alpha <= 0) return;
    constexpr unsigned white = 0xF0F1DF;
    const double w = shape.width * scale, h = 30 * scale;
    RoundBox(ring, -w / 2, -h / 2, w, h, (shape.pad ? 12 : 5) * scale, 2 * scale, color, alpha, .06);
    // The press lights the cap from inside.
    if (flash > 0) RoundBox(ring, -w / 2, -h / 2, w, h, (shape.pad ? 12 : 5) * scale, 0, white, 0, flash * alpha);
}
// Reused ActionScript arrays and gradient box for beginGradientFill: made once with the menu and refilled per call,
// so drawing a gradient allocates nothing per frame.
struct Paint {
    RE::GFxValue colors, alphas, ratios, box;
    bool ready = false;
};
struct Stop {
    double at;
    unsigned color;
    double alpha;
};
// Starts a gradient fill over the box (x, y, w, h): "linear" runs left to right, "radial" fills the ellipse inside
// the box, edges padded with the last stop. Reversed runs a linear gradient right to left.
bool BeginGradient(RE::GFxValue& clip, Paint& paint, const char* type, std::initializer_list<Stop> stops, double x,
    double y, double w, double h, bool reversed = false) {
    if (!paint.ready || w <= 0 || h <= 0) return false;
    const auto n = static_cast<std::uint32_t>(stops.size());
    paint.colors.SetArraySize(n);
    paint.alphas.SetArraySize(n);
    paint.ratios.SetArraySize(n);
    std::uint32_t i = 0;
    for (const auto& stop : stops) {
        const auto slot = reversed ? n - 1 - i : i;
        paint.colors.SetElement(slot, RE::GFxValue(stop.color));
        paint.alphas.SetElement(slot, RE::GFxValue(std::clamp(stop.alpha, 0.0, 1.0) * 100));
        paint.ratios.SetElement(slot, RE::GFxValue(std::clamp(reversed ? 1 - stop.at : stop.at, 0.0, 1.0) * 255));
        ++i;
    }
    paint.box.SetMember("x", RE::GFxValue(x));
    paint.box.SetMember("y", RE::GFxValue(y));
    paint.box.SetMember("w", RE::GFxValue(w));
    paint.box.SetMember("h", RE::GFxValue(h));
    clip.Invoke("lineStyle", nullptr, nullptr, 0);
    const RE::GFxValue args[]{RE::GFxValue(type), paint.colors, paint.alphas, paint.ratios, paint.box};
    return clip.Invoke("beginGradientFill", nullptr, args, 5);
}
void RectPath(RE::GFxValue& clip, double x, double y, double w, double h) {
    const double points[][2] = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}, {x, y}};
    for (unsigned i = 0; i < 5; ++i) {
        const RE::GFxValue p[]{RE::GFxValue(points[i][0]), RE::GFxValue(points[i][1])};
        clip.Invoke(i ? "lineTo" : "moveTo", nullptr, p, 2);
    }
    clip.Invoke("endFill", nullptr, nullptr, 0);
}
// A horizontal line of height h along y from x0 to x1, its stops running from x0 to x1 (either way round).
void HBand(
    RE::GFxValue& clip, Paint& paint, double x0, double x1, double y, double h, std::initializer_list<Stop> stops) {
    const double left = std::min(x0, x1), width = std::abs(x1 - x0);
    if (BeginGradient(clip, paint, "linear", stops, left, y - h / 2, width, h, x1 < x0))
        RectPath(clip, left, y - h / 2, width, h);
}
// A soft light: full at (x, y), gone at the ellipse rx by ry.
void Glow(RE::GFxValue& clip, Paint& paint, double x, double y, double rx, double ry, unsigned color, double alpha) {
    alpha = std::min(1.0, alpha * QteConverge::kGlowGain);
    if (alpha <= .002 || rx <= 0 || ry <= 0) return;
    if (BeginGradient(clip, paint, "radial", {{0, color, alpha}, {1, color, 0}}, x - rx, y - ry, 2 * rx, 2 * ry))
        RectPath(clip, x - rx, y - ry, 2 * rx, 2 * ry);
}
void Dot(RE::GFxValue& clip, double x, double y, double r, unsigned color, double alpha) {
    clip.Invoke("lineStyle", nullptr, nullptr, 0);
    const RE::GFxValue fill[]{RE::GFxValue(color), RE::GFxValue(std::clamp(alpha, 0.0, 1.0) * 100)};
    clip.Invoke("beginFill", nullptr, fill, 2);
    const double c = r / std::cos(std::numbers::pi / 8);
    const RE::GFxValue start[]{RE::GFxValue(x + r), RE::GFxValue(y)};
    clip.Invoke("moveTo", nullptr, start, 2);
    for (int i = 1; i <= 8; ++i) {
        const double a = i * std::numbers::pi / 4, m = a - std::numbers::pi / 8;
        const RE::GFxValue p[]{RE::GFxValue(x + c * std::cos(m)), RE::GFxValue(y + c * std::sin(m)),
            RE::GFxValue(x + r * std::cos(a)), RE::GFxValue(y + r * std::sin(a))};
        clip.Invoke("curveTo", nullptr, p, 4);
    }
    clip.Invoke("endFill", nullptr, nullptr, 0);
}
void Diamond(RE::GFxValue& clip, double r, double width, unsigned color, double alpha) {
    if (alpha <= .002) return;
    const RE::GFxValue style[]{
        RE::GFxValue(width), RE::GFxValue(color), RE::GFxValue(std::clamp(alpha, 0.0, 1.0) * 100)};
    clip.Invoke("lineStyle", nullptr, style, 3);
    const double points[][2] = {{-r, 0}, {0, -r}, {r, 0}, {0, r}, {-r, 0}};
    for (unsigned i = 0; i < 5; ++i) {
        const RE::GFxValue p[]{RE::GFxValue(points[i][0]), RE::GFxValue(points[i][1])};
        clip.Invoke(i ? "lineTo" : "moveTo", nullptr, p, 2);
    }
}
constexpr unsigned warm = 0xFFF0D7, gold = 0xFFD08C;
// One converging line from its edge xe to its head xh at brightness a (the mockup's hairTail).
void HairTail(RE::GFxValue& glow, Paint& paint, double xe, double xh, double a) {
    const double dir = xh >= xe ? 1 : -1, len = std::abs(xh - xe);
    if (len < 2 || a <= 0) return;
    const double x0 = len > 1000 ? xh - dir * 1000 : xe;
    HBand(glow, paint, x0, xh, 0, 1.2, {{0, warm, 0}, {.75, warm, .18 * a}, {1, warm, .55 * a}});
    HBand(glow, paint, xh - dir * std::min(len, 190.0), xh, 0, 2.6, {{0, warm, 0}, {1, warm, a}});
    const double s1 = xh - dir * std::min(len, 420.0), e1 = xh - dir * std::min(len, 30.0);
    for (double oy : {-6.0, 6.0}) HBand(glow, paint, s1, e1, oy, 1, {{0, warm, 0}, {1, warm, .13 * a}});
    // The mockup's bloom, as soft light: a long, low halo trailing the head and a round one on it.
    const double trail = std::min(len, 190.0);
    if (BeginGradient(glow, paint, "radial", {{0, warm, .22 * a}, {1, warm, 0}}, xh - 190, -10, 380, 20))
        RectPath(glow, dir > 0 ? xh - trail : xh, -10, trail, 20);
    Glow(glow, paint, xh, 0, 24, 24, warm, .45 * a);
    Dot(glow, xh, 0, 2.4, 0xFFFFFF, a);
}
// Thin strokes in a few alpha steps, so each step sets its line style once (sparks, ash).
struct Stroke {
    float x, y, xx, yy, alpha;
};
void Strokes(RE::GFxValue& glow, std::span<const Stroke> strokes, double width, unsigned color, const char* caps) {
    constexpr int steps = 10;
    for (int step = 1; step <= steps; ++step) {
        bool styled = false;
        for (const auto& s : strokes) {
            if (static_cast<int>(std::ceil(s.alpha * steps - .001)) != step) continue;
            if (!styled) {
                const RE::GFxValue style[]{RE::GFxValue(width), RE::GFxValue(color), RE::GFxValue(step * 100.0 / steps),
                    RE::GFxValue(false), RE::GFxValue("normal"), RE::GFxValue(caps)};
                glow.Invoke("lineStyle", nullptr, style, 6);
                styled = true;
            }
            const RE::GFxValue from[]{RE::GFxValue(s.x), RE::GFxValue(s.y)},
                to[]{RE::GFxValue(s.xx), RE::GFxValue(s.yy)};
            glow.Invoke("moveTo", nullptr, from, 2);
            glow.Invoke("lineTo", nullptr, to, 2);
        }
    }
}
// The converging lines (ui/QteConverge.h, the user's QTE_Mockups design 1). The ring clip holds the darkening around
// the band; its "glow" child, blended additively, holds every line and light. Drawing units are pixels at 1080 lines
// with the middle at 0; reach is how far the lines start, bounds the screen's edges (left, top, right, bottom).
void DrawConverge(RE::GFxValue& ring, RE::GFxValue& glow, Paint& paint, const Qte::View& v, double reach,
    const std::array<double, 4>& bounds) {
    const auto& s = v.spec;
    const auto& clock = v.clock;
    const double ui = QteConverge::Ui(s, clock), focus = QteConverge::Focus(s, clock);
    const auto [left, top, right, bottom] = bounds;
    if (focus > .002) {
        if (BeginGradient(ring, paint, "radial", {{.2, 0, 0}, {1, 0, .85 * focus}}, -1250, -60 - 1250, 2500, 2500))
            RectPath(ring, left, top, right - left, bottom - top);
        // A dimmer band along the lines: a very wide ellipse fades only with height across the screen.
        if (BeginGradient(ring, paint, "radial", {{0, 0, .42 * focus}, {1, 0, 0}}, -20000, -190, 40000, 380))
            RectPath(ring, left, -190, right - left, 380);
    }
    if (ui <= 0) return;
    HBand(glow, paint, -reach, 0, 0, 1, {{0, warm, 0}, {1, warm, .07 * ui}});
    HBand(glow, paint, 0, reach, 0, 1, {{0, warm, .07 * ui}, {1, warm, 0}});
    const auto timing = QteConverge::TimingOf(s);
    const double head = QteConverge::Head(s, clock), rt = clock.resultElapsed;
    if (clock.outcome == QteOutcome::None) {
        const double wp = reach * timing.perfect / timing.approach, wg = reach * timing.success / timing.approach;
        for (double side : {-1.0, 1.0}) {
            if (wp > 0) Line(glow, side * wp, -7, side * wp, 7, 1, warm, .38 * ui);
            Line(glow, side * wg, -3.5, side * wg, 3.5, 1, warm, .16 * ui);
        }
        if (head > 0) {
            const double a = .55 + .45 * QteConverge::EaseIn(head);
            HairTail(glow, paint, -reach, -reach * (1 - head), a);
            HairTail(glow, paint, reach, reach * (1 - head), a);
        }
        Diamond(glow, 38, 1.3, warm, .75 * ui);
        if (head > 0) Diamond(glow, std::max(4.0, 38 + 130 * (1 - head)), 1, warm, .5 * QteConverge::EaseIn(head));
        return;
    }
    if (clock.outcome != QteOutcome::Failure) {
        const double q = QteConverge::Quality(clock.outcome);
        const double fl = std::exp(-rt * 3.2) * q, hl = reach * QteConverge::EaseOut5(rt / .16);
        if (hl > 1) {
            HBand(glow, paint, -hl, hl, 0, 2.4, {{0, gold, 0}, {.5, 0xFFFAF0, fl}, {1, gold, 0}});
            HBand(glow, paint, -hl, hl, 0, 20, {{0, gold, 0}, {.5, 0xFFFAF0, fl * .16}, {1, gold, 0}});
        }
        Glow(glow, paint, 0, 0, 110, 110, gold, .8 * q * std::exp(-rt * 5));
        Glow(glow, paint, 0, 0, 28, 28, 0xFFFFFF, q * std::exp(-rt * 7));
        const double vs = 120 * q * std::exp(-rt * 6);
        if (vs > .5) Line(glow, 0, -vs, 0, vs, 1.4, 0xFFF8EB, std::exp(-rt * 6));
        std::array<Stroke, 64> sparks;
        std::size_t count = 0;
        const int total = QteConverge::SparkCount(clock.outcome);
        for (int i = 0; i < total && count < sparks.size(); ++i) {
            const auto spark = QteConverge::SparkOf(v.session, i, clock.outcome);
            if (rt >= spark.life) continue;
            const auto p = QteConverge::Fly(0, 0, spark.vx, spark.vy, 3.2, 60, rt);
            sparks[count++] = {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.x - p.vx * .025),
                static_cast<float>(p.y - p.vy * .025), static_cast<float>(1 - rt / spark.life)};
        }
        Strokes(glow, std::span<const Stroke>(sparks.data(), count), 1.3, gold, "round");
        const double e = QteConverge::EaseOut(rt / .55);
        Diamond(glow, 38 * (1 + 1.7 * e), 1.5, gold, (1 - e) * q);
        Diamond(glow, 38, 1.3, gold, .85 * ui);
        return;
    }
    // A miss: the lines flicker and go out where they stopped, crumble to ash, and the diamond breaks in two.
    const double xl = -reach * (1 - head), xr = reach * (1 - head);
    const double flicker = rt < .2 ? (QteConverge::Hash(9, static_cast<std::uint32_t>(rt * 50)) > .45 ? 1 : .25) : 1;
    const double a = flicker * (1 - QteConverge::EaseOut((rt - .12) / .45));
    HairTail(glow, paint, -reach, xl, a);
    HairTail(glow, paint, reach, xr, a);
    if (rt > .12) {
        const double age = rt - .12, span = std::min(800.0, reach * head);
        std::array<Stroke, QteConverge::kAshCount> ash;
        std::size_t count = 0;
        for (int i = 0; i < QteConverge::kAshCount; ++i) {
            const auto fleck = QteConverge::AshOf(v.session, i);
            if (age >= fleck.life) continue;
            const double back = fleck.back * span;
            const double alpha = QteConverge::Clamp01((age - back / 800 * .35) * 6) * (1 - age / fleck.life) * .75;
            if (alpha <= 0) continue;
            const double x = fleck.side > 0 ? xl - back : xr + back;
            const auto p = QteConverge::Fly(x, fleck.y, fleck.vx, fleck.vy, .6, -6, age);
            const double px = p.x + std::sin(rt * 2 + fleck.wobble) * 3;
            ash[count++] = {static_cast<float>(px), static_cast<float>(p.y), static_cast<float>(px + 1.7),
                static_cast<float>(p.y), static_cast<float>(alpha)};
        }
        Strokes(glow, std::span<const Stroke>(ash.data(), count), 1.7, 0xCDC3B4, "none");
    }
    const double sp = 9 * QteConverge::EaseOut(rt / .4), da = ui * (1 - QteConverge::EaseOut((rt - .25) / 1));
    if (da > .002) {
        const unsigned color = rt < .35 ? 0xE68C78 : 0xAAA096;
        for (double side : {-1.0, 1.0}) {
            const RE::GFxValue style[]{RE::GFxValue(1.3), RE::GFxValue(color), RE::GFxValue(.8 * da * 100)};
            glow.Invoke("lineStyle", nullptr, style, 3);
            const double points[][2] = {{-38 - sp * .3, side * sp}, {0, side * (38 + sp)}, {38 + sp * .3, side * sp}};
            for (unsigned i = 0; i < 3; ++i) {
                const RE::GFxValue p[]{RE::GFxValue(points[i][0]), RE::GFxValue(points[i][1])};
                glow.Invoke(i ? "lineTo" : "moveTo", nullptr, p, 2);
            }
        }
    }
}
bool TextField(RE::GFxValue& clip, RE::GFxValue& text, const char* name, int depth) {
    const RE::GFxValue args[]{RE::GFxValue(name), RE::GFxValue(depth), RE::GFxValue(-180), RE::GFxValue(130),
        RE::GFxValue(360), RE::GFxValue(24)};
    if (!clip.Invoke("createTextField", nullptr, args, 6) || !clip.GetMember(name, &text)) return false;
    text.SetMember("selectable", RE::GFxValue(false));
    return true;
}
std::string KeyLabel(QteKey key) {
    std::string text;
    // Labels are prepared once per session, never in the input hook.
    const std::pair<unsigned, const char*> labels[] = {{18, "E"}, {16, "Q"}, {19, "R"}, {17, "W"}, {30, "A"}, {31, "S"},
        {32, "D"}, {33, "F"}, {34, "G"}, {35, "H"}, {36, "J"}, {37, "K"}, {38, "L"}, {44, "Z"}, {45, "X"}, {46, "C"},
        {47, "V"}, {48, "B"}, {49, "N"}, {50, "M"}, {20, "T"}, {21, "Y"}, {22, "U"}, {23, "I"}, {24, "O"}, {25, "P"},
        {57, "Space"}, {28, "Enter"}, {256, "LMB"}, {257, "RMB"}, {258, "MMB"}};
    for (auto [code, name] : labels)
        if (code == key.key) text = name;
    if (text.empty() && key.key) text = "Key " + std::to_string(key.key);
    if (key.gamepad) {
        if (!text.empty()) text += " / ";
        const char* label = nullptr;
        for (auto [code, name] : {std::pair{4096u, "A"}, {8192u, "B"}, {16384u, "X"}, {32768u, "Y"}, {256u, "LB"},
                 {512u, "RB"}, {1u, "Up"}, {2u, "Down"}, {4u, "Left"}, {8u, "Right"}, {16u, "Start"}, {32u, "Back"},
                 {64u, "LS"}, {128u, "RS"}, {9u, "LT"}, {10u, "RT"}})
            if (code == key.gamepad) label = name;
        text += "Pad ";
        text += label ? label : std::to_string(key.gamepad);
    }
    return text;
}
// A key with both a keyboard/mouse key and a pad button (an attack QTE, or a key set for both) is named
// by the device in use only; a key set for one device keeps it.
QteKey Shown(QteKey key, bool pad) {
    if (!key.key || !key.gamepad) return key;
    return pad ? QteKey{0, key.gamepad} : QteKey{key.key, 0};
}
void Draw(RE::GFxValue& ring, const Qte::View& v, const KeyShape& key) {
    const auto& s = v.spec;
    const auto& clock = v.clock;
    const double p = std::clamp(clock.Progress(s), 0.0f, 1.0f), g = std::clamp(clock.Gauge(s), 0.0f, 1.0f),
                 target = QteClock::target;
    constexpr unsigned white = 0xF0F1DF, green = 0xA1C8AD, gold = 0xF2D795, dim = 0x59615F;
    const bool result = clock.outcome != QteOutcome::None;
    const unsigned color = !result                                ? white
                           : clock.outcome == QteOutcome::Failure ? 0xEF9590
                           : clock.outcome == QteOutcome::Perfect ? gold
                                                                  : green;
    const double alpha =
        result ? 1 - std::clamp(static_cast<double>(clock.resultElapsed) / std::max(.001f, s.feedback), 0.0, 1.0) : 1;
    const auto mode = QteFace::ModeOf(s);
    const auto look = QteFace::LookOf(s, clock);
    if (mode == QteFace::Mode::Converge) {
        // Drawn by DrawConverge.
    } else if (s.type == QteType::SplitArc) {
        KeyCap(ring, key, 1, color, look.alpha, 0);
        if (!result) {
            Arc(ring, 49, 0, tau, 138 * s.successWindow, green, .18);
            if (s.perfectWindow > 0) Arc(ring, 49, 0, tau, 138 * s.perfectWindow, gold, .35);
            Ring(ring, 49, .1, 3, green, .85);
            Arc(ring, 92 - 69 * p, 0, tau, 2, white, .95);
        } else {
            const double t = s.reducedMotion ? .28
                                             : std::clamp(clock.resultElapsed /
                                                              std::max(.001, static_cast<double>(s.feedback) * .85),
                                                   0.0, 1.0),
                         e = 1 - std::pow(1 - t, 3);
            Ring(ring, 49 + 52 * e, .1, 3, color, 1 - t);
            if (clock.outcome == QteOutcome::Perfect) Arc(ring, 49 + 39 * e, 0, tau, 2, color, (1 - t) * .7);
        }
    } else if (s.type == QteType::TimingBar) {
        Box(ring, -120, -10, 240, 20, dim, .5 * alpha);
        Box(ring, -120 + 240 * std::max(0.0, target - s.successWindow), -10,
            240 * (std::min(1.0, target + s.successWindow) - std::max(0.0, target - s.successWindow)), 20, green,
            .7 * alpha);
        Box(ring, -120 + 240 * (target - s.perfectWindow), -10, 480 * s.perfectWindow, 20, gold, alpha);
        Line(ring, -120 + 240 * p, -18, -120 + 240 * p, 18, 3, color, alpha);
    } else if (s.type == QteType::RotatingNeedle || s.type == QteType::HoldRelease) {
        Arc(ring, 66, -tau / 4, 3 * tau / 4, 3, dim, alpha);
        Arc(ring, 66, std::max(0.0, target - s.successWindow) * tau - tau / 4,
            std::min(1.0, target + s.successWindow) * tau - tau / 4, 12, green, .6 * alpha);
        if (s.perfectWindow > 0)
            Arc(ring, 66, std::max(0.0, target - s.perfectWindow) * tau - tau / 4,
                std::min(1.0, target + s.perfectWindow) * tau - tau / 4, 12, gold, alpha);
        if (s.type == QteType::HoldRelease) {
            if (g > 0) Arc(ring, 54, -tau / 4, g * tau - tau / 4, 5, color, alpha);
        } else {
            const double a = p * tau - tau / 4;
            Line(ring, 0, 0, 60 * std::cos(a), 60 * std::sin(a), 3, color, alpha);
        }
    } else if (mode == QteFace::Mode::Single) {
        KeyCap(ring, key, QteFace::kSingleScale * look.scale, white, look.alpha, look.flash);
    } else if (s.type == QteType::RapidPress) {
        const unsigned count = std::min(20u, s.pressCount);
        const double width = 240.0 / count;
        for (unsigned i = 0; i < count; ++i)
            Box(ring, -120 + i * width, -10, width - 3, 20, (i + 1.0) / count <= g ? color : dim, alpha);
    } else {
        const double start = -static_cast<double>(s.sequenceCount) * 44 / 2;
        for (unsigned i = 0; i < s.sequenceCount; ++i) {
            const unsigned c = i < clock.completed ? green : i == clock.completed ? color : dim;
            Box(ring, start + i * 44, -20, 40, 40, c, (i == clock.completed ? .28 : .12) * alpha);
            Line(ring, start + i * 44, 22, start + i * 44 + 40, 22, 2, c, alpha);
        }
    }
    if (mode == QteFace::Mode::Hint &&
        (s.type == QteType::HoldRelease || s.type == QteType::RapidPress || s.type == QteType::KeySequence)) {
        Line(ring, -120, 88, 120, 88, 2, dim, alpha);
        Line(ring, -120, 88, -120 + 240 * (1 - p), 88, 2, color, alpha);
    }
}
class Menu final : public RE::IMenu {
public:
    static RE::IMenu* Create() { return new Menu; }
    Menu() {
        depthPriority = 101;
        menuFlags.set(RE::UI_MENU_FLAGS::kAlwaysOpen, RE::UI_MENU_FLAGS::kRequiresUpdate,
            RE::UI_MENU_FLAGS::kAllowSaving, RE::UI_MENU_FLAGS::kAdvancesUnderPauseMenu);
        inputContext = Context::kNone;
        auto* manager = RE::BSScaleformManager::GetSingleton();
        // Reuse the packaged empty movie; no external UI dependency is required.
        if (!manager || !manager->LoadMovie(this, uiMovie, "AnimationInjectorLoading") || !uiMovie) {
            logger::error("[QTE] missing Interface/AnimationInjectorLoading.swf");
            return;
        }
        RE::GFxValue root;
        // This movie owns its scaling; other menus and HUD mods are untouched.
        uiMovie->SetViewScaleMode(RE::GFxMovieView::ScaleModeType::kNoScale);
        uiMovie->SetViewAlignment(RE::GFxMovieView::AlignType::kTopLeft);
        ready = uiMovie->GetVariable(&root, "_root");
        // One clip per QTE channel, each with its own hint and key labels.
        for (std::size_t c = 0; ready && c < slots.size(); ++c) {
            auto& slot = slots[c];
            const auto clip = "qte" + std::to_string(c);
            ready = root.CreateEmptyMovieClip(&slot.ring, clip.c_str(), static_cast<std::int32_t>(1 + c));
            // The converging lines' lights, added onto what lies beneath (under the key's name).
            if (ready) ready = slot.ring.CreateEmptyMovieClip(&slot.glow, "glow", 1);
            if (ready) slot.glow.SetMember("blendMode", RE::GFxValue("add"));
            if (ready) ready = TextField(slot.ring, slot.hint, "hint", 2);
            for (unsigned i = 0; ready && i < slot.labels.size(); ++i) {
                const auto label = "key" + std::to_string(i);
                ready = TextField(slot.ring, slot.labels[i], label.c_str(), 3 + i);
            }
        }
        if (ready) {
            uiMovie->CreateArray(&paint.colors);
            uiMovie->CreateArray(&paint.alphas);
            uiMovie->CreateArray(&paint.ratios);
            uiMovie->CreateObject(&paint.box);
            paint.ready = paint.colors.IsArray() && paint.alphas.IsArray() && paint.ratios.IsArray() &&
                          paint.box.IsObject() && paint.box.SetMember("matrixType", RE::GFxValue("box")) &&
                          paint.box.SetMember("r", RE::GFxValue(0.0));
            if (!paint.ready)
                logger::warn("[QTE] gradient arrays unavailable; converging lines draw without gradients");
        }
        root.Invoke("stop");
        uiMovie->SetVisible(false);
    }
    RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage& message) override {
        if (message.type == RE::UI_MESSAGE_TYPE::kHide) return RE::UI_MESSAGE_RESULTS::kIgnore;
        if (message.type == RE::UI_MESSAGE_TYPE::kScaleformEvent) return RE::UI_MESSAGE_RESULTS::kPassOn;
        return RE::IMenu::ProcessMessage(message);
    }
    void AdvanceMovie(float delta, std::uint32_t) override {
        if (!ready || !uiMovie) return;
        const auto views = Qte::ReadViews();
        auto* ui = RE::UI::GetSingleton();
        const bool showing = ui && ui->IsShowingMenus() && !ui->GameIsPaused();
        visible = false;
        // A hidden QTE (0.12.266) takes input with nothing drawn.
        for (const auto& v : views) visible |= showing && v.visible && !v.spec.hidden;
        uiMovie->SetVisible(visible);
        if (!visible) return;
        const auto rect = uiMovie->GetVisibleFrameRect();
        RE::GViewport viewport;
        uiMovie->GetViewport(&viewport);
        for (std::size_t c = 0; c < slots.size(); ++c) DrawSlot(slots[c], views[c], viewport, rect, paint);
        uiMovie->Advance(std::isfinite(delta) ? std::clamp(delta, 0.0f, .1f) : 0);
    }
    void PostDisplay() override {
        if (ready && visible && uiMovie) uiMovie->Display();
    }

private:
    struct Slot {
        RE::GFxValue ring;
        RE::GFxValue glow;
        RE::GFxValue hint;
        std::array<RE::GFxValue, 8> labels;
        QteOutcome lastOutcome = QteOutcome::None;
        unsigned lastCompleted = ~0u;
        bool lastHolding = false;
        std::uint64_t session = 0;
        bool pad = false;  // The device the key's name was made for.
        // The key's shape and its label's place this session (the circle's middle, a one-press QTE).
        KeyShape key;
        double labelY = 100, labelSize = 13;
    };
    static void DrawSlot(
        Slot& slot, const Qte::View& v, const RE::GViewport& viewport, const RE::GRectF& rect, Paint& paint) {
        if (!v.visible || v.spec.hidden) {
            slot.ring.SetMember("_visible", RE::GFxValue(false));
            return;
        }
        const auto mode = QteFace::ModeOf(v.spec);
        // Names are made at a session's start and again only when the player switches device.
        const bool pad = QteServices::UsingGamepad();
        if (slot.session != v.session || slot.pad != pad) {
            slot.session = v.session;
            slot.pad = pad;
            const QteKey own = Shown({v.spec.key, v.spec.gamepad}, pad);
            const auto ownText = KeyLabel(own);
            slot.key = {std::max(30.0, ownText.size() * 9.0 + 16), own.gamepad && !own.key};
            // The circle and a one-press QTE draw the key in the middle (a mouse button by its name, LMB/RMB/MMB).
            slot.labelSize = mode == QteFace::Mode::Single     ? 13 * QteFace::kSingleScale * 1.1
                             : mode == QteFace::Mode::Circle   ? 15
                             : mode == QteFace::Mode::Converge ? 30
                                                               : 13;
            slot.labelY = mode == QteFace::Mode::Hint ? 100 : -slot.labelSize * .78;
            for (unsigned i = 0; i < slot.labels.size(); ++i) {
                const bool sequence = v.spec.type == QteType::KeySequence;
                const bool used = sequence ? i < v.spec.sequenceCount : i == 0;
                const auto keyText = KeyLabel(sequence ? Shown(v.spec.sequence[i], pad) : own);
                char color[16];
                std::snprintf(color, sizeof(color), "#%06X",
                    mode == QteFace::Mode::Converge ? QteConverge::KeyColor(QteOutcome::None) : 0xF0F1DFu);
                const auto html = used ? std::string("<p align='center'><font face='$EverywhereFont' size='") +
                                             (sequence ? "10" : std::to_string(static_cast<int>(slot.labelSize))) +
                                             "' color='" + color + "'>" + keyText + "</font></p>"
                                       : "";
                slot.labels[i].SetTextHTML(html.c_str());
                RE::GFxValue::DisplayInfo pos;
                pos.SetX(sequence ? -static_cast<double>(v.spec.sequenceCount) * 44 / 2 + i * 44 : -150);
                pos.SetY(sequence ? -14 : slot.labelY);
                pos.SetXScale(100);
                pos.SetYScale(100);
                pos.SetAlpha(100);
                slot.labels[i].SetDisplayInfo(pos);
                slot.labels[i].SetMember("_width", RE::GFxValue(sequence ? 40 : 300));
                slot.labels[i].SetMember("_height", RE::GFxValue(sequence ? 44 : slot.labelSize * 1.6 + 4));
            }
            slot.lastOutcome = QteOutcome::None;
            slot.lastCompleted = ~0u;
            slot.lastHolding = false;
        }
        // Placed by what this QTE draws, its key included (the key's shape is made above, once per session).
        const auto placement = QteLayout::Calculate(viewport.width, viewport.height, v.spec.scale, v.spec.x, v.spec.y,
            v.spec.anchor, QteLayout::BoundsOf(v.spec, slot.key.width));
        const auto movie = QteLayout::ToMovie(placement, viewport.width, viewport.height, rect.left, rect.top,
            rect.right - rect.left, rect.bottom - rect.top);
        const bool shown = movie.scaleX > 0 && movie.scaleY > 0;
        slot.ring.SetMember("_visible", RE::GFxValue(shown));
        if (!shown) return;
        RE::GFxValue::DisplayInfo pos;
        pos.SetX(movie.x);
        pos.SetY(movie.y);
        pos.SetXScale(movie.scaleX);
        pos.SetYScale(movie.scaleY);
        slot.ring.SetDisplayInfo(pos);
        slot.ring.Invoke("clear");
        slot.glow.Invoke("clear");
        Draw(slot.ring, v, slot.key);
        if (mode == QteFace::Mode::Converge) {
            const double unit = placement.unit;
            const std::array<double, 4> bounds{-placement.x / unit, -placement.y / unit,
                (viewport.width - placement.x) / unit, (viewport.height - placement.y) / unit};
            DrawConverge(slot.ring, slot.glow, paint, v, QteConverge::Reach(viewport.width, placement.x, unit), bounds);
        }
        if (mode != QteFace::Mode::Hint) {
            // The key's name follows its cap: the press scales it about the middle, a result fades it.
            const auto look = QteFace::LookOf(v.spec, v.clock);
            RE::GFxValue::DisplayInfo label;
            label.SetX(-150 * look.scale);
            label.SetY(slot.labelY * look.scale);
            label.SetXScale(100 * look.scale);
            label.SetYScale(100 * look.scale);
            label.SetAlpha(100 * look.alpha);
            slot.labels[0].SetDisplayInfo(label);
        }
        if (slot.lastOutcome != v.clock.outcome || slot.lastCompleted != v.clock.completed ||
            slot.lastHolding != v.clock.holding) {
            slot.lastOutcome = v.clock.outcome;
            slot.lastCompleted = v.clock.completed;
            slot.lastHolding = v.clock.holding;
            if (mode != QteFace::Mode::Hint) {
                // No text: the circle shows its result in colour (the key's name too), a one-press QTE shows none.
                slot.hint.SetTextHTML("");
                if (mode == QteFace::Mode::Circle || mode == QteFace::Mode::Converge) {
                    const unsigned tint = mode == QteFace::Mode::Converge ? QteConverge::KeyColor(slot.lastOutcome)
                                          : slot.lastOutcome == QteOutcome::None    ? 0xF0F1DF
                                          : slot.lastOutcome == QteOutcome::Failure ? 0xEF9590
                                          : slot.lastOutcome == QteOutcome::Perfect ? 0xF2D795
                                                                                    : 0xA1C8AD;
                    char html[160];
                    std::snprintf(html, sizeof(html),
                        "<p align='center'><font face='$EverywhereFont' size='%d' color='#%06X'>",
                        static_cast<int>(slot.labelSize), tint);
                    const auto text =
                        std::string(html) + KeyLabel(Shown({v.spec.key, v.spec.gamepad}, slot.pad)) + "</font></p>";
                    slot.labels[0].SetTextHTML(text.c_str());
                }
                return;
            }
            char text[256];
            const char* state = slot.lastOutcome == QteOutcome::Perfect   ? "PERFECT"
                                : slot.lastOutcome == QteOutcome::Success ? "SUCCESS"
                                : slot.lastOutcome == QteOutcome::Failure ? "FAILED"
                                : v.spec.type == QteType::HoldRelease
                                    ? (slot.lastHolding ? "RELEASE IN TARGET" : "HOLD, THEN RELEASE")
                                : v.spec.type == QteType::RapidPress  ? "TAP REPEATEDLY"
                                : v.spec.type == QteType::KeySequence ? "PRESS IN ORDER"
                                                                      : "PRESS IN TARGET";
            if (slot.lastOutcome == QteOutcome::None &&
                (v.spec.type == QteType::RapidPress || v.spec.type == QteType::KeySequence))
                std::snprintf(text, sizeof(text),
                    "<p align='center'><font face='$EverywhereFont' size='13' color='#F0F1DF'>%s %u / %u</font></p>",
                    state, slot.lastCompleted,
                    v.spec.type == QteType::RapidPress ? v.spec.pressCount : v.spec.sequenceCount);
            else
                std::snprintf(text, sizeof(text),
                    "<p align='center'><font face='$EverywhereFont' size='13' color='#F0F1DF'>%s</font></p>", state);
            slot.hint.SetTextHTML(text);
        }
    }
    std::array<Slot, Qte::kChannels> slots;
    Paint paint;
    bool ready = false, visible = false;
};
}
void Show() {
    if (!registered) return;
    auto* ui = RE::UI::GetSingleton();
    auto* queue = RE::UIMessageQueue::GetSingleton();
    static const RE::BSFixedString menuName(name);
    if (ui && queue && !ui->IsMenuOpen(name)) queue->AddMessage(menuName, RE::UI_MESSAGE_TYPE::kShow, nullptr);
}
void Register() {
    if (registered) return;
    auto* ui = RE::UI::GetSingleton();
    if (!ui) return;
    ui->Register(name, Menu::Create);
    registered = true;
    Show();
}
}
