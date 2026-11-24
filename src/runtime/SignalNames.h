#pragma once
#include <string>
#include <string_view>

// The OAR signal names (runtime/Signals.h), apart from the game so the declaration parser and its tests can use them.
namespace AnimationInjector::Signals {

// A signal name as the author writes it (<Action signal="Charge">): 1~32 of A-Z a-z 0-9 _.
inline bool ValidName(std::string_view name) {
    if (name.empty() || name.size() > 32) return false;
    for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

// The graph variable an OAR condition reads: "bAI_" + the signal name (user decision 11-24: short, without the set's
// id). One name is one variable for every mod: authors keep theirs apart by a prefix of their own (MyBow_Charge).
inline std::string VariableName(std::string_view name) {
    std::string out = "bAI_";
    out.append(name);
    return out;
}

}  // namespace AnimationInjector::Signals
