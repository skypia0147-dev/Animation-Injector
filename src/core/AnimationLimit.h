#pragma once
#include <algorithm>
#include <cstdint>
#include <string_view>

// How many animation bindings a behavior project may hold, decided once per game session.
//
// The engine stores a clip's binding index in 16 bits and reads it signed: 32,767 at most. OAR's
// [Experimental] bIncreaseAnimationLimit patches the engine to read it unsigned (65,534 at most;
// 0xFFFF stays "no binding"), and OAR's [General] uAnimationLimit is the count OAR stops the game
// above. This module follows both, so animation mods keep working in load orders past 32,767.
namespace AnimationInjector::AnimationLimit {

inline constexpr int kSignedMaxIndex = 0x7FFF;
inline constexpr int kUnsignedMaxIndex = 0xFFFE;
// Synchronized clips add bindings after the names, so names stop this far below the cap.
inline constexpr int kSynchronizedReserve = 1024;

// The two loads OAR rewrites in hkbClipGenerator::activate (+0x160 and +0x176): movsx (0F BF)
// becomes movzx (0F B7). Only both rewritten counts as unsigned: any other bytes mean another
// build or another patch, and the signed limit stays.
constexpr bool ReadsUnsigned(const std::uint8_t (&first)[2], const std::uint8_t (&second)[2]) {
    return first[0] == 0x0F && first[1] == 0xB7 && second[0] == 0x0F && second[1] == 0xB7;
}

// [General] uAnimationLimit from the text of OAR's INI, or -1 when it is not there. OAR saves the
// file with a UTF-8 byte order mark, which GetPrivateProfileString does not understand (it then
// misses the first section, [General]), so the file is parsed here: sections and keys ignore case,
// lines starting with ';' or '#' are comments.
constexpr int OarIniLimit(std::string_view text) {
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    const auto same = [&](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (lower(a[i]) != lower(b[i])) return false;
        return true;
    };
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    const auto trim = [&](std::string_view v) {
        while (!v.empty() && blank(v.front())) v.remove_prefix(1);
        while (!v.empty() && blank(v.back())) v.remove_suffix(1);
        return v;
    };
    bool general = false;
    while (!text.empty()) {
        const auto eol = text.find('\n');
        const std::string_view line = trim(text.substr(0, eol));
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[') {
            const auto close = line.find(']');
            general = close != std::string_view::npos && same(trim(line.substr(1, close - 1)), "General");
            continue;
        }
        const auto eq = line.find('=');
        if (!general || eq == std::string_view::npos || !same(trim(line.substr(0, eq)), "uAnimationLimit")) continue;
        const std::string_view value = trim(line.substr(eq + 1));
        long n = 0;
        std::size_t i = 0;
        for (; i < value.size() && value[i] >= '0' && value[i] <= '9' && n <= 0xFFFF; ++i)
            n = n * 10 + (value[i] - '0');
        return i > 0 && n > 0 ? static_cast<int>(std::min<long>(n, 0xFFFF)) : -1;
    }
    return -1;
}

struct Limits {
    bool unsignedIndex = false;  // the engine reads binding indices unsigned
    int engineMaxIndex = kSignedMaxIndex;
    int oarLimit = -1;                 // OAR's uAnimationLimit when OAR is loaded, else -1
    int bindingCap = kSignedMaxIndex;  // bindings a project may hold
    int maxBindingIndex = kSignedMaxIndex;
    int nameTableLimit = kSignedMaxIndex - kSynchronizedReserve;
};

// The limits from the engine's index reading and OAR's limit (-1 without OAR).
constexpr Limits Compute(bool unsignedIndex, int oarLimit) {
    Limits l;
    l.unsignedIndex = unsignedIndex;
    l.engineMaxIndex = unsignedIndex ? kUnsignedMaxIndex : kSignedMaxIndex;
    l.oarLimit = oarLimit;
    // OAR counts bindings and stops the game above its limit; without OAR the engine's largest index decides.
    l.bindingCap = oarLimit > 0 ? std::min(l.engineMaxIndex, oarLimit) : l.engineMaxIndex;
    l.maxBindingIndex = oarLimit > 0 ? std::min(l.engineMaxIndex, oarLimit - 1) : l.engineMaxIndex;
    l.nameTableLimit = std::max(0, l.bindingCap - kSynchronizedReserve);
    return l;
}

// Resolved on first use (after every plugin's load, so OAR's patch is in place) and logged once.
const Limits& Get();
inline int MaxBindingIndex() { return Get().maxBindingIndex; }
inline int NameTableLimit() { return Get().nameTableLimit; }

}
