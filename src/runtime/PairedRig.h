#pragma once
#include <string_view>
#include <string>
#include "runtime/PairedRigCatalog.h"

namespace AnimationInjector::PairedRig {

// Character-data names and race project filenames are different namespaces.
// Keep explicit profiles so another creature cannot inherit a human role.
constexpr bool Equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}
constexpr const Profile* Find(Kind kind) {
    const auto index = static_cast<unsigned>(kind);
    return index > 0 && index <= profiles.size() ? &profiles[index - 1] : nullptr;
}
constexpr Kind FromName(std::string_view name) {
    for (const auto& profile : profiles)
        if (Equal(name, profile.name)) return profile.kind;
    return Kind::kUnsupported;
}
constexpr Kind CharacterName(std::string_view name) {
    if (name.empty()) return Kind::kUnsupported;
    for (const auto& profile : profiles)
        if (Equal(name, profile.character) || Equal(name, profile.characterAlias)) return profile.kind;
    return Kind::kUnsupported;
}
constexpr Kind ProjectPath(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    const auto file = path.substr(slash == path.npos ? 0 : slash + 1);
    if (file.empty()) return Kind::kUnsupported;
    for (const auto& profile : profiles)
        if (Equal(file, profile.project) || Equal(file, profile.projectAlias)) return profile.kind;
    return Kind::kUnsupported;
}
constexpr std::uint64_t Mask(Kind kind) {
    return Find(kind) ? std::uint64_t{1} << (static_cast<unsigned>(kind) - 1) : 0;
}
constexpr bool IsCreature(Kind kind) { return kind != Kind::kCharacter && Find(kind); }
// Draugr and skeleton share a behavior template and asset root. Register both
// declarations consistently before their shared template is linked.
constexpr bool SharesAssets(Kind a, Kind b) {
    const auto* first = Find(a);
    const auto* second = Find(b);
    return first && second && Equal(first->actorRoot, second->actorRoot);
}
constexpr bool SupportsRole(Kind target, Kind project, int variant) {
    return variant == 0 ? project == target && target != Kind::kUnsupported
                        : variant == 1 && project == Kind::kCharacter;
}
constexpr const char* Name(Kind kind) {
    const auto* profile = Find(kind);
    return profile ? profile->name : "unsupported";
}
// XML clips are relative to the human character asset root. Creature graphs
// resolve filenames from their own asset roots; point them at the same HKX.
inline std::string ClipPath(std::string_view file, Kind rig) {
    const auto* profile = Find(rig);
    if (!profile || rig == Kind::kCharacter || file.empty()) return std::string(file);
    constexpr std::string_view actors = "meshes/actors/";
    const std::string_view root = profile->actorRoot;
    if (!root.starts_with(actors)) return std::string(file);
    std::string path = "..\\";
    for (char c : root.substr(actors.size()))
        if (c == '/' || c == '\\') path += "..\\";
    path += "character\\";
    path.append(file);
    return path;
}
}
