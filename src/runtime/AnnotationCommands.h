#pragma once
#include "PCH.h"
#include "runtime/EventRegistrySpec.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

// Commands a clip's annotation carries under the reserved event "AI" (0.12.274), the way Payload Interpreter
// reads "PIE.@COMMAND|...": the engine splits "AI.CamShake|1.5|0.5" at its first '.' into the event "AI" and
// the payload "CamShake|1.5|0.5". Any animation may carry one (an action's, a pair's, another mod's); no XML
// declares it, and only "AI" itself is in the graphs' event tables, so the numbers are free.
//
// Every event function but a strike has a command (11-02, docs/mockup-ai-commands in the editor), run on the
// actor that sent it with the annotation's own values: no registration, the same in any HKX.
//   IFrameOn[|span] IFrameOff  ArmorOn[|span] ArmorOff  RotateOn[|span] RotateOff
//   TimeOn|factor|ease[|span] TimeOff  Hide  Show  MagicAdd|plugin|form  MagicRemove|plugin|form
//   MagicCast|plugin|form  Cost|stamina|magicka|health  CostPercent|stamina|magicka|health
//   CamShake|strength|duration  StopVelocity  Ragdoll|direction|force  FireArrow|damage%
//   Sound|file|bone|volume  SoundForm|plugin|form|bone|volume  GlobalSet|plugin|form|value
//   ModEvent|name|text|number  CancelPair
// A window's on turned on without a span follows the window rules (runtime/EventEffects.h): its off command is
// always there to send.
namespace AnimationInjector::AnnotationCommands {
// The reserved event name. Actions::RegistryEventNames always holds it.
inline constexpr std::string_view kEvent = "AI";

// A command: the function it runs, with the annotation's values.
using Parsed = EventFunctionSpec;

// Pure (tests). "Name|value|...": the name ignores case, each value is a whole field (a number std::from_chars
// reads), and every value is written (0.12.275: no defaults, what the annotation says is what runs); a window's
// on may add its span (0~60 seconds) last. Empty for anything else: an unknown name, a missing, empty or extra
// field, a number that is not finite or out of its range, a choice not among its names.
inline std::optional<Parsed> Parse(std::string_view payload) {
    std::array<std::string_view, 7> v{};
    std::size_t count = 0;
    for (;;) {
        if (count == v.size()) return std::nullopt;
        const auto bar = payload.find('|');
        v[count++] = payload.substr(0, bar);
        if (bar == std::string_view::npos) break;
        payload.remove_prefix(bar + 1);
    }
    const std::size_t values = count - 1;
    const auto same = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    };
    const auto number = [](std::string_view text, float minimum, float maximum, float& out) {
        if (text.empty()) return false;
        float value = 0.0f;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) return false;
        if (value < minimum || value > maximum) return false;
        out = value;
        return true;
    };
    const auto plugin = [](std::string_view text, std::string& out) {
        if (text.empty() || text.find_first_of("/\\:") != std::string_view::npos) return false;
        out = text;
        return true;
    };
    // A form's local hex id, as <Do form>.
    const auto form = [](std::string_view text, std::string& out) {
        if (text.starts_with("0x") || text.starts_with("0X")) text.remove_prefix(2);
        if (text.empty() || text.size() > 8 ||
            !std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }))
            return false;
        out = text;
        return true;
    };
    // A sound file: a path under Data, as <Do file>.
    const auto file = [](std::string_view text, std::string& out) {
        if (text.empty() || text.find(':') != std::string_view::npos || text.starts_with("\\") ||
            text.starts_with("/") || text.find("..") != std::string_view::npos)
            return false;
        out = text;
        return true;
    };
    const auto bone = [](std::string_view text, std::string& out) {
        for (std::string_view b : {"spine", "head", "rightHand", "leftHand", "root"})
            if (text == b) {
                out = text;
                return true;
            }
        return false;
    };
    const auto direction = [](std::string_view text, RagdollPush::Direction& out) {
        for (std::size_t i = 0; i < RagdollPush::kNames.size(); ++i)
            if (text == RagdollPush::kNames[i]) {
                out = static_cast<RagdollPush::Direction>(i);
                return true;
            }
        return false;
    };
    constexpr float kAny = std::numeric_limits<float>::max();
    Parsed f;
    // `n` values; a window's on may add its span after them.
    const auto plain = [&](EventOp op, std::size_t n) {
        f.op = op;
        return values == n;
    };
    const auto window = [&](EventOp op, std::size_t n) {
        f.op = op;
        return values == n || (values == n + 1 && number(v[n + 1], 0.0f, 60.0f, f.span));
    };
    const auto name = v[0];
    bool ok = false;
    if (same(name, "IFrameOn"))
        ok = window(EventOp::kIFrameOn, 0);
    else if (same(name, "IFrameOff"))
        ok = plain(EventOp::kIFrameOff, 0);
    else if (same(name, "ArmorOn"))
        ok = window(EventOp::kArmorOn, 0);
    else if (same(name, "ArmorOff"))
        ok = plain(EventOp::kArmorOff, 0);
    else if (same(name, "RotateOn"))
        ok = window(EventOp::kRotateOn, 0);
    else if (same(name, "RotateOff"))
        ok = plain(EventOp::kRotateOff, 0);
    else if (same(name, "TimeOn"))
        ok = window(EventOp::kTimeOn, 2) && number(v[1], 0.05f, 5.0f, f.factor) && number(v[2], 0.0f, 2.0f, f.ease);
    else if (same(name, "TimeOff"))
        ok = plain(EventOp::kTimeOff, 0);
    else if (same(name, "Hide"))
        ok = plain(EventOp::kHide, 0);
    else if (same(name, "Show"))
        ok = plain(EventOp::kShow, 0);
    else if (same(name, "MagicAdd") || same(name, "MagicRemove") || same(name, "MagicCast"))
        ok = plain(same(name, "MagicAdd")      ? EventOp::kMagicAdd
                   : same(name, "MagicRemove") ? EventOp::kMagicRemove
                                               : EventOp::kMagicCast,
                 2) &&
             plugin(v[1], f.plugin) && form(v[2], f.form);
    else if (same(name, "Cost") || same(name, "CostPercent")) {
        f.costPercent = same(name, "CostPercent");
        const float most = f.costPercent ? 100.0f : 100000.0f;
        ok = plain(EventOp::kCost, 3) && number(v[1], 0.0f, most, f.stamina) && number(v[2], 0.0f, most, f.magicka) &&
             number(v[3], 0.0f, most, f.health);
    } else if (same(name, "CamShake"))
        ok =
            plain(EventOp::kShake, 2) && number(v[1], 0.0f, 10.0f, f.strength) && number(v[2], 0.0f, 30.0f, f.duration);
    else if (same(name, "StopVelocity"))
        ok = plain(EventOp::kStopVelocity, 0);
    else if (same(name, "Ragdoll"))
        ok = plain(EventOp::kRagdoll, 2) && direction(v[1], f.direction) &&
             number(v[2], 0.0f, RagdollPush::kMaxForce, f.force);
    else if (same(name, "FireArrow")) {
        float damage = 0.0f;
        ok = plain(EventOp::kFireArrow, 1) && number(v[1], 1.0f, 1000.0f, damage);
        f.arrowDamage = damage / 100.0f;
    } else if (same(name, "Sound"))
        ok =
            plain(EventOp::kSound, 3) && file(v[1], f.file) && bone(v[2], f.bone) && number(v[3], 0.0f, 1.0f, f.volume);
    else if (same(name, "SoundForm"))
        ok = plain(EventOp::kSound, 4) && plugin(v[1], f.plugin) && form(v[2], f.form) && bone(v[3], f.bone) &&
             number(v[4], 0.0f, 1.0f, f.volume);
    else if (same(name, "GlobalSet"))
        ok = plain(EventOp::kGlobalSet, 3) && plugin(v[1], f.plugin) && form(v[2], f.form) &&
             number(v[3], -kAny, kAny, f.value);
    else if (same(name, "ModEvent")) {
        ok = plain(EventOp::kModEvent, 3) && !v[1].empty() && number(v[3], -kAny, kAny, f.number);
        f.name = v[1];
        f.text = v[2];
    } else if (same(name, "CancelPair"))
        ok = plain(EventOp::kCancel, 0);
    if (!ok) return std::nullopt;
    return f;
}

// Pure (tests). A paired clip often carries a command for both roles at the same time ("NPCAI.CamShake|…" and
// "2_AI.CamShake|…"): the second of the pair within `kWindow` seconds is the same moment and does not run again.
class PairEcho {
public:
    static constexpr double kWindow = 0.15;
    // `actor` sent `command` at `now` (seconds); `partner` is its pair partner (0 outside a pair). True when
    // the partner sent the same command within the window: skip it. Records what runs.
    bool Repeat(std::uint32_t actor, std::uint32_t partner, const Parsed& command, double now) {
        std::erase_if(recent, [&](const Entry& e) { return now - e.at > kWindow || now < e.at; });
        if (partner)
            for (const auto& e : recent)
                if (e.actor == partner && e.partner == actor && e.command == command) return true;
        recent.push_back({actor, partner, command, now});
        return false;
    }
    void Clear() { recent.clear(); }

private:
    struct Entry {
        std::uint32_t actor, partner;
        Parsed command;
        double at;
    };
    std::vector<Entry> recent;
};

// Any thread (the animation event hook, every actor): the payload of an "AI" event `actor` sent. A valid
// command is copied into a bounded queue with its sender; an invalid text is logged once per distinct text
// (at most 32 kept). Nothing about the actor is changed here.
void Observe(RE::Actor* actor, const RE::BSFixedString& payload);

// Main thread, with EventRegistry::Update. A command runs on its sender, as a registered event's function. A
// CamShake shakes the camera at the sender's position when the sender is the player or the player's pair partner
// (a pair's victim clip may carry it), others' are dropped, and a pair's second role sending the same one at the
// same moment too (PairEcho).
void Update();

// Load / new game: drops the queue (a command sent before the load is not run after it).
void ResetOnLoad();
}
