#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

namespace AnimationInjector {
// Case-insensitive clip name -> (action, variant). The motion and clip hooks ask this for every
// clip of every actor. Injected clips are always named "AI_<id>_Clip<tag>" (ActionSpec::Finalize),
// so any other name, i.e. every vanilla clip, is rejected without touching the table.
class ClipIndex {
public:
    template<class Specs>
    void Build(const Specs& specs) {
        _entries.clear();
        for (int a = 0; a < static_cast<int>(specs.size()); ++a)
            for (int v = 0; v < static_cast<int>(specs[a].clips.size()); ++v)
                _entries.try_emplace(Lower(specs[a].clips[v].clipName), Entry{a, v});  // First wins, like a scan.
    }
    bool Find(const char* name, int& action, int& variant) const {
        if (!name || (name[0] | 0x20) != 'a' || (name[1] | 0x20) != 'i' || name[2] != '_') return false;
        const auto it = _entries.find(std::string_view(name));
        if (it == _entries.end()) return false;
        action = it->second.action;
        variant = it->second.variant;
        return true;
    }
    std::size_t Size() const { return _entries.size(); }

private:
    struct Entry {
        int action, variant;
    };
    static char Fold(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }
    static std::string Lower(std::string_view text) {
        std::string out(text);
        for (auto& c : out) c = Fold(c);
        return out;
    }
    // Heterogeneous lookup: hash and compare the caller's text case-insensitively without copying it.
    struct Hash {
        using is_transparent = void;
        std::size_t operator()(std::string_view text) const noexcept {
            std::size_t h = 1469598103934665603ull;
            for (const char c : text) {
                h ^= static_cast<unsigned char>(Fold(c));
                h *= 1099511628211ull;
            }
            return h;
        }
    };
    struct Equal {
        using is_transparent = void;
        bool operator()(std::string_view a, std::string_view b) const noexcept {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i)
                if (Fold(a[i]) != Fold(b[i])) return false;
            return true;
        }
    };
    std::unordered_map<std::string, Entry, Hash, Equal> _entries;
};
}
