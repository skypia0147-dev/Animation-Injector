#include "core/VanillaLadders.h"

#include <cctype>
#include <deque>
#include <mutex>
#include <string>

namespace AnimationInjector::VanillaLadders {
namespace {
const std::vector<Ladder>& All() {
    static const std::vector<Ladder> ladders{
#include "core/VanillaLadders.inl"
    };
    return ladders;
}
bool Same(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}
}  // namespace

const Ladder* Find(std::string_view graph, std::string_view blender) {
    for (const auto& l : All())
        if (Same(l.blender, blender) && Same(l.graph, graph)) return &l;
    // An own tree's blender (core/OwnLocomotion): 2HW_<Direction>Blend is the 2HM record renamed, H2H_<Direction>Blend
    // the 1HM one (built once per blender, kept for the process).
    static constexpr std::pair<std::string_view, std::string_view> kOwn[] = {{"2HW_", "2HM_"}, {"H2H_", "1HM_"}};
    for (const auto& [to, from] : kOwn) {
        if (blender.size() <= 4 || !Same(blender.substr(0, 4), to)) continue;
        const std::string sourceName = std::string(from) + std::string(blender.substr(4));
        const Ladder* source = Find(graph, sourceName);
        if (!source) return nullptr;
        static std::mutex mutex;
        static std::deque<Ladder> made;
        static std::deque<std::string> names;
        std::lock_guard lock(mutex);
        for (const auto& l : made)
            if (Same(l.blender, blender) && Same(l.graph, graph)) return &l;
        Ladder copy = *source;
        copy.blender = names.emplace_back(std::string(to) + std::string(blender.substr(4))).c_str();
        for (auto& slot : copy.slots) {
            std::string clip = slot.clip;
            if (clip.size() > 4 && Same(std::string_view(clip).substr(0, 4), from)) clip.replace(0, 4, to);
            slot.clip = names.emplace_back(std::move(clip)).c_str();
        }
        return &made.emplace_back(std::move(copy));
    }
    return nullptr;
}

}  // namespace AnimationInjector::VanillaLadders
