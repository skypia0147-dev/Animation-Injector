#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

// Which windows a clip turns on and off by itself (user decision 11-02): a registered event that turns a window on,
// with an event that turns it off later in the same clip, makes the window last only as long as that clip, as a clip
// range does. Pure, for the tests; runtime/ClipWindows.h applies it to the playing clips.
namespace AnimationInjector::ClipWindowsJudge {
// The windows: damage immunity, hyper armor, rotation, time scale.
constexpr std::size_t kWindows = 4;
// The "event" of an AI. annotation command (runtime/AnnotationCommands.h): not a registry index.
constexpr std::size_t kCommand = static_cast<std::size_t>(-1);
// A registered event's part in the windows, as bits by window: the ones it turns on and the ones it turns off.
struct Role {
    std::size_t event = 0;  // the registry's event index
    std::uint8_t on = 0, off = 0;
};
// A clip annotation naming a registered event with a part in the windows.
struct Note {
    float time = 0.0f;
    const Role* role = nullptr;
};
struct Own {
    std::size_t event = 0;
    std::size_t window = 0;
    bool operator==(const Own&) const = default;
};
// A paired clip's annotation names the actor it is for by a prefix ("NPC": the support, the attacker; "2_": the
// lead, the victim), and the engine sends it to that actor without it. The part of `text` it is for (1 "NPC", 2
// "2_"), the prefix taken off; 0 and `text` as it is without one.
inline int PairPart(std::string_view& text) {
    for (const auto& [prefix, part] : {std::pair{std::string_view("NPC"), 1}, std::pair{std::string_view("2_"), 2}})
        if (text.size() > prefix.size() && text.substr(0, prefix.size()) == prefix) {
            text.remove_prefix(prefix.size());
            return part;
        }
    return 0;
}
// The part of a synchronized clip whose prefix is `prefix` (a null one is "NPC", as the engine reads it); 0 for any
// other prefix (an empty one reads its annotations as written).
inline int PartOfPrefix(const char* prefix) {
    const std::string_view p = prefix ? prefix : "NPC";
    return p == "NPC" ? 1 : p == "2_" ? 2 : 0;
}
// The (event, window) pairs the notes turn on with that window's off event at a later time.
inline std::vector<Own> Judge(const std::vector<Note>& notes) {
    std::vector<Own> own;
    for (const auto& on : notes)
        for (std::size_t w = 0; w < kWindows; ++w) {
            const auto bit = static_cast<std::uint8_t>(1u << w);
            if (!(on.role->on & bit)) continue;
            const bool later = std::any_of(notes.begin(), notes.end(),
                [&](const Note& off) { return off.time > on.time && (off.role->off & bit); });
            const Own pair{on.role->event, w};
            if (later && std::find(own.begin(), own.end(), pair) == own.end()) own.push_back(pair);
        }
    return own;
}
}
