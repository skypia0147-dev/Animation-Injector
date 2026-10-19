#pragma once
#include <string>
#include <string_view>
#include <cstdint>
#include <vector>

namespace AnimationInjector {
// <Hold> inside <Carry>: the carrier's upper-body pose while carrying (stage 2).
struct CarryHoldSpec {
    struct Bone {
        std::string name;
        float weight = 1;  // 0..1; inherited by descendants until another Bone overrides it
    };
    // Empty = no hold. "lift-end" = the lift clip's last frame (the only pose the grammar accepts).
    std::string pose;
    float blendIn = 0.2f, blendOut = 0.2f;
    std::vector<Bone> bones;
    bool Enabled() const {
        if (pose.empty()) return false;
        for (const auto& bone : bones)
            if (bone.weight > 0) return true;
        return false;
    }
};
// What a drawn weapon means to a carry.
enum class CarryWeapon : std::uint8_t {
    kSheathed,  // default: starts only sheathed; drawing a weapon or spell drops the target
    kSheathe,   // may start with a weapon drawn: as the lift starts the carrier's weapon goes to its
                // sheath with no animation, then it counts as kSheathed (drawing a weapon or spell
                // drops the target). Until then, and if that fails, the carrier's weapons are hidden and
                // an attack or a spell cast drops the target
};
// The XML value.
inline const char* CarryWeaponName(CarryWeapon weapon) {
    return weapon == CarryWeapon::kSheathe ? "sheathe" : "sheathed";
}
// How the carried target is held once the lift ends (carry stage 3).
enum class CarryTargetMode : std::uint8_t {
    kPose,     // the lift's last pose, frozen (default; also the fallback)
    kPhysics,  // one ragdoll body fixed to the carrier bone, every other body simulates
};
struct CarrySpec {
    bool enabled = false;
    std::string bone, targetRoot;
    // A one-actor Action the carrier plays to put the target down; its CarryRelease slot marks
    // the moment the target is released as a ragdoll (docs/설계-운반-래그돌-내려놓기.md).
    std::string putdownAction;
    std::uint32_t key = 0xFFFFFFFF, gamepad = 0;
    CarryHoldSpec hold;
    CarryTargetMode mode = CarryTargetMode::kPose;
    // kPhysics: the target bone whose ragdoll body stays fixed. Empty = the body nearest the
    // carrier bone when the target attaches.
    std::string part;
    // Seconds into the lift clip when the target is fixed: its pose is locked and its root follows
    // the carrier bone for the rest of the lift. < 0 = when the lift ends (its Exit), as before.
    float fixAt = -1.0f;
    CarryWeapon weapon = CarryWeapon::kSheathed;
};
inline bool ValidCarryBone(std::string_view name) {
    if (name.empty() || name.size() > 256 || name.front() == ' ' || name.back() == ' ') return false;
    for (unsigned char c : name)
        if (c < 32 || c == 127) return false;
    return true;
}
}
