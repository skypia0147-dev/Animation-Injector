#pragma once
#include <charconv>
#include <cstdint>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

// Payload Interpreter's native commands, run by the runtime when Payload Interpreter is not installed (1.1.0).
//
// Clips made for MCO and its movesets carry annotations for Payload Interpreter under its event "PIE":
//   PIE.@SGVI|MCO_nextattack|2      set a graph int variable
//   PIE.@SGVF|MCO_AttackSpeed|1.0   set a graph float variable
//   PIE.@CAMSHAKE|0.5|0.3           shake the camera (strength, seconds)
// The engine splits such an annotation at its first '.' into the event "PIE" and the payload, as it does for the
// runtime's own "AI" commands (runtime/AnnotationCommands.h). With these three handled here, a moveset whose clips
// use nothing else of Payload Interpreter plays without it: the MCO add-on reads the variables the clips set.
//
// Payload Interpreter itself is left alone. When its DLL is loaded the runtime does nothing with "PIE": every
// command runs once, by its owner. Its other commands and a mod's own macros ("PIE.$Name") are not handled here;
// a mod that needs them brings its own handler or Payload Interpreter.
//
// The variables: one a check reads or a file registers (<VariableRegistry>) is in the graphs already; one that is
// not there is not created, and the write is simply refused by the graph, as Payload Interpreter's would be.
namespace AnimationInjector::PayloadCommands {
// The event name. It is in every graph's event table (Actions::RegistryEventNames), so the clips' annotations
// reach the runtime without a behavior patch.
inline constexpr std::string_view kEvent = "PIE";

enum class Kind : std::uint8_t { kSetInt, kSetFloat, kCamShake };
struct Parsed {
    Kind kind = Kind::kSetInt;
    std::string variable;  // kSetInt, kSetFloat
    int integer = 0;       // kSetInt
    float number = 0.0f;   // kSetFloat: the value; kCamShake: the strength
    float seconds = 0.0f;  // kCamShake
};

// Pure (tests). The command's name ignores case; every field is written and is a whole number std::from_chars
// reads. Empty for anything else: another command, a macro ("$Name"), a missing or extra field, a number that is
// not finite.
inline std::optional<Parsed> Parse(std::string_view payload) {
    std::string_view f[4];
    std::size_t count = 0;
    for (;;) {
        if (count == 4) return std::nullopt;
        const auto bar = payload.find('|');
        f[count++] = payload.substr(0, bar);
        if (bar == std::string_view::npos) break;
        payload.remove_prefix(bar + 1);
    }
    if (count != 3) return std::nullopt;
    const auto same = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            const char c = a[i] >= 'a' && a[i] <= 'z' ? static_cast<char>(a[i] - 'a' + 'A') : a[i];
            if (c != b[i]) return false;
        }
        return true;
    };
    const auto real = [](std::string_view text, float& out) {
        if (text.empty()) return false;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
        return ec == std::errc{} && end == text.data() + text.size() && std::isfinite(out);
    };
    Parsed p;
    if (same(f[0], "@SGVI")) {
        if (f[1].empty() || f[2].empty()) return std::nullopt;
        const auto [end, ec] = std::from_chars(f[2].data(), f[2].data() + f[2].size(), p.integer);
        if (ec != std::errc{} || end != f[2].data() + f[2].size()) return std::nullopt;
        p.kind = Kind::kSetInt;
        p.variable = f[1];
        return p;
    }
    if (same(f[0], "@SGVF")) {
        if (f[1].empty() || !real(f[2], p.number)) return std::nullopt;
        p.kind = Kind::kSetFloat;
        p.variable = f[1];
        return p;
    }
    if (same(f[0], "@CAMSHAKE")) {
        if (!real(f[1], p.number) || !real(f[2], p.seconds) || p.number < 0.0f || p.seconds < 0.0f) return std::nullopt;
        p.kind = Kind::kCamShake;
        return p;
    }
    return std::nullopt;
}

#ifndef AI_PAYLOAD_COMMANDS_PURE
// Any thread (the animation event sink): the payload of a "PIE" event `actor` sent. Queued for Update.
void Observe(RE::Actor* actor, const RE::BSFixedString& payload);
// Main thread, once a frame: the queued commands, run on their senders.
void Update();
// Loads: what is queued is dropped.
void ResetOnLoad();
#endif
}  // namespace AnimationInjector::PayloadCommands
