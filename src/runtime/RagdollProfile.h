#pragma once
#include "core/CarryPartialRagdoll.h"
#include "runtime/PairedRig.h"
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AnimationInjector::RagdollProfile {
// Per-rig physics values for a carried target (carry stage 3), written by HKX_Edit's ragdoll
// workspace or by hand. Format: docs/설계-래그돌-프로필과-미리보기.md, chapter 1.
//
//   <Ragdoll rig="chicken" version="1">
//     <Motion follow="0.4" gravity="1.5" drag="0.3" spin="3" inertia="3" friction="1"/>
//     <Body bone="LeftAnkle" swingFrom="LeftThigh" cone="35" plane="-20 20" twist="-10 10"/>
//   </Ragdoll>
// Where a profile came from: the lift clip's own file, or the rig's shared one.
enum class Source : std::uint8_t { kClip, kRig };
struct Profile {
    CarryPartialRagdoll::Dynamics motion;
    std::vector<CarryPartialRagdoll::BodyTuning> bodies;
    std::string source;  // file path, for logs
    Source from = Source::kRig;
};
// Data/SKSE/Plugins/AnimationInjector/<kFolder>/ holds every profile; the declaration scan skips it.
//   <rig>.xml                  every carry of that rig
//   <lift clip file name>.xml  only carries lifted by that clip, e.g. Grapple_Lift.xml for
//                              clip="Animations\Grapple\Grapple_Lift.hkx" (same-named clips share it)
inline constexpr std::wstring_view kFolder = L"Ragdoll";
// Rig profile names HKX_Edit can save beyond the runtime's rigs; no lift clip profile takes them.
inline constexpr std::array<std::string_view, 2> kHkxEditRigs{"character_female", "draugr_f"};
// Strict: an unknown element or attribute, a value out of range or a repeated bone rejects the
// whole file with the reason in `error`. `rig` empty accepts any rig name.
std::optional<Profile> Parse(const std::filesystem::path& file, std::string_view rig, std::string& error);
// <kFolder>/<lift clip file name>.xml, relative to the AnimationInjector folder. Empty when the clip
// cannot name one: not .hkx, an empty, "." or ".." name, or a rig profile's name (a rig the runtime
// knows or kHkxEditRigs).
std::filesystem::path ClipProfilePath(std::string_view liftClip);
// The lift clip's profile, else the rig's, under `data` (a Data folder). A clip profile for another
// rig, or a rejected one, falls back to the rig's (logged). nullopt: neither.
std::optional<Profile> Resolve(const std::filesystem::path& data, PairedRig::Kind rig, std::string_view liftClip);
// Resolve under the game's Data, at every pickup, so an edited file applies on the next lift. Main thread.
std::optional<Profile> Load(PairedRig::Kind rig, std::string_view liftClip);
}
