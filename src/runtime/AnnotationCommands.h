#pragma once
#include "PCH.h"
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <optional>
#include <string_view>
#include <vector>

// Commands a clip's annotation carries under the reserved event "AI" (0.12.274), the way Payload Interpreter
// reads "PIE.@COMMAND|...": the engine splits "AI.CamShake|1.5|0.5" at its first '.' into the event "AI" and
// the payload "CamShake|1.5|0.5". Any animation may carry one (an action's, a pair's, another mod's); no XML
// declares it, and only "AI" itself is in the graphs' event tables, so the numbers are free.
namespace AnimationInjector::AnnotationCommands {
// The reserved event name. Actions::RegistryEventNames always holds it.
inline constexpr std::string_view kEvent = "AI";

enum class Command : std::uint8_t { kCamShake };

struct Parsed {
    Command command = Command::kCamShake;
    float strength = 0.0f;  // CamShake: [0, 10]
    float duration = 0.0f;  // CamShake: seconds, [0, 30]
    bool operator==(const Parsed&) const = default;
};

// Pure (tests). "CamShake|strength|duration": the command name ignores case, each number is a whole field
// std::from_chars reads, and every number is written (0.12.275: no defaults, what the annotation says is what
// runs). Empty for anything else: an unknown command, a missing, empty or extra field, a number that is not
// finite or out of its range.
inline std::optional<Parsed> Parse(std::string_view payload) {
    std::array<std::string_view, 3> fields{};
    std::size_t count = 0;
    for (;;) {
        if (count == fields.size()) return std::nullopt;
        const auto bar = payload.find('|');
        fields[count++] = payload.substr(0, bar);
        if (bar == std::string_view::npos) break;
        payload.remove_prefix(bar + 1);
    }
    const auto same = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    };
    const auto number = [](std::string_view text, float maximum, float& out) {
        if (text.empty()) return false;
        float value = 0.0f;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) return false;
        if (value < 0.0f || value > maximum) return false;
        out = value;
        return true;
    };
    if (count != 3 || !same(fields[0], "CamShake")) return std::nullopt;
    Parsed out;
    if (!number(fields[1], 10.0f, out.strength) || !number(fields[2], 30.0f, out.duration)) return std::nullopt;
    return out;
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

// Main thread, with EventRegistry::Update. CamShake shakes the camera at the sender's position when the
// sender is the player or the player's pair partner (a pair's victim clip may carry it); other actors'
// commands are dropped, and a pair's second role sending the same command at the same moment too (PairEcho).
void Update();

// Load / new game: drops the queue (a command sent before the load is not run after it).
void ResetOnLoad();
}
