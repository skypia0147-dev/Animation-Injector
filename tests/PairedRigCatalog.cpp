#include "runtime/PairedRig.h"
#include <iostream>
#include <string>
#include <cstdlib>
#include <filesystem>
using namespace AnimationInjector::PairedRig;
void require(bool value) {
    if (!value) std::abort();
}
int main() {
    std::uint64_t seen = 0;
    require(profiles.size() == 47);
    for (const auto& p : profiles) {
        require(FromName(p.name) == p.kind);
        require(CharacterName(p.character) == p.kind);
        require(ProjectPath(std::string(p.actorRoot) + "/" + p.project) == p.kind);
        require(Mask(p.kind) != 0 && !(seen & Mask(p.kind)));
        seen |= Mask(p.kind);
        require(SupportsRole(p.kind, p.kind, 0));
        require(SupportsRole(p.kind, Kind::kCharacter, 1));
        if (IsCreature(p.kind)) {
            require(!SupportsRole(p.kind, Kind::kCharacter, 0));
            require(!SupportsRole(p.kind, p.kind, 1));
            require(p.root[0] && p.wrapper[0] && p.activity[0] && p.eventProbe[0]);
            for (const auto* probe : p.probes) require(probe[0]);
        }
        for (const auto& other : profiles)
            if (p.kind != other.kind) {
                require(!SupportsRole(p.kind, other.kind, 0));
                require(!Equal(p.name, other.name));
            }
    }
    require((seen >> 32) != 0);
    require(FromName("CHICKEN") == Kind::kChicken);
    require(CharacterName("ChickenProject") == Kind::kUnsupported);
    require(CharacterName("ChickenCharater") == Kind::kChicken);
    require(ProjectPath("UnknownCreature.hkx") == Kind::kUnsupported);
    require(ProjectPath("") == Kind::kUnsupported && CharacterName("") == Kind::kUnsupported);
    require(Mask(static_cast<Kind>(65)) == 0);
    require(SharesAssets(FromName("draugr"), FromName("skeleton")));
    require(!SharesAssets(FromName("bear"), FromName("wolf")));
    const std::string clip = "Animations\\AIHumanChicken\\ChickenCarry_Lower.hkx";
    require(ClipPath(clip, Kind::kChicken) == "..\\..\\character\\" + clip);
    require(ClipPath(clip, Kind::kCharacter) == clip);
    require(ClipPath(clip, Kind::kDraugr) == "..\\character\\" + clip);
    const auto expected = (std::filesystem::path("meshes/actors/character") / clip).lexically_normal();
    for (const auto& profile : profiles) {
        const auto actual =
            (std::filesystem::path(profile.actorRoot) / ClipPath(clip, profile.kind)).lexically_normal();
        require(actual == expected);
    }
    require(ClipPath("", Kind::kChicken).empty());
    std::cout << "All 47 project roots resolve the same human HKX; nested chicken/DLC roots passed\n";
    std::cout << "47 profiles: identity, role separation, unique 64-bit masks and shared roots passed\n";
}
