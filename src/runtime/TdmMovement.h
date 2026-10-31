#pragma once
#include <optional>
#include <string_view>

namespace AnimationInjector::TdmMovement {
// The player's movement direction as the character moves (10-30). True Directional Movement, when installed, has a
// directional setting in which the player turns toward the movement input and always runs forward: there a movement
// condition reads forward whatever key is held. Its other settings (off, vanilla style facing the crosshair) and its
// target lock move the player the way the keys point, as without TDM.
// The setting is read from TDM's own settings (MCM ini uDirectionalMovementDrawn / uDirectionalMovementSheathed, per
// the weapon's state), not from TDM's API mode: TDM implements vanilla style as directional movement that faces the
// crosshair only while it sees movement input, so its API answers "directional" whenever it sees none (user report
// 10-30: inside a charge every key read forward with the weapon drawn in vanilla style).

// What TDM is set to now, or kAbsent without TDM.
enum class Mode { kAbsent, kDisabled, kVanillaStyle, kDirectional, kTargetLock };

// The direction a movement condition reads, from the keys' direction (MoveDir: 0 forward, 1 back, 2 left, 3 right,
// 4 none; -1 unknown).
inline int ForCharacter(int keys, Mode mode) {
    constexpr int kForward = 0, kNone = 4;
    return mode == Mode::kDirectional && keys >= 0 && keys != kNone ? kForward : keys;
}

// TDM's setting value (DirectionalMovementMode: 0 disabled, 1 vanilla style, 2 directional) as our mode.
inline Mode FromSetting(int setting) {
    return setting == 2 ? Mode::kDirectional : setting == 1 ? Mode::kVanillaStyle : Mode::kDisabled;
}

// A whole-number value of an INI text: `key = value` under `[section]` (case as written; a UTF-8 BOM, spaces and
// `;` / `#` comment lines allowed). None when absent or not a number.
inline std::optional<int> IniInt(std::string_view text, std::string_view section, std::string_view key) {
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    const auto trim = [](std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
        return s;
    };
    bool inside = false;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[') {
            inside = line.size() >= 2 && line.back() == ']' && line.substr(1, line.size() - 2) == section;
            continue;
        }
        const auto equals = line.find('=');
        if (!inside || equals == std::string_view::npos || trim(line.substr(0, equals)) != key) continue;
        const auto value = trim(line.substr(equals + 1));
        if (value.empty()) return std::nullopt;
        int out = 0;
        for (const char c : value) {
            if (c < '0' || c > '9') return std::nullopt;
            out = out * 10 + (c - '0');
        }
        return out;
    }
    return std::nullopt;
}

// Finds TDM once the game data is in (kDataLoaded): every plugin is loaded by then.
void Load();
// TDM's setting for this weapon state, or kTargetLock while it has a target locked; kAbsent before Load or without
// TDM. `refresh`: re-read its settings file first (at most once a second, so an MCM change applies); the per-frame
// read leaves that to the captures.
Mode Current(bool weaponDrawn, bool refresh = false);
}  // namespace AnimationInjector::TdmMovement
