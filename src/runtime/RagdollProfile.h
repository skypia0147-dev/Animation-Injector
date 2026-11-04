#pragma once
#include "core/CarryPartialRagdoll.h"
#include "runtime/PairedRig.h"
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AnimationInjector::RagdollProfile {
// Physics values for a carried target (carry stage 3): one file per lift action, next to its action XML.
//   <mod folder>/<any>.xml                 the action XML (AnimationSet)
//   <mod folder>/Ragdoll/<lift id>.xml     that file's lift action <lift id>, every value written
// Written by the editor (and its ragdoll simulation) or by hand, read at every pickup. Without a file, or with a
// rejected one, vanilla (BuiltIn). Format: docs/설계-래그돌-프로필과-미리보기.md, chapter 1.
//
//   <Ragdoll rig="chicken" version="1">
//     <Motion follow="0.4" gravity="1.5" drag="0.3" spin="3" inertia="3" friction="1"/>
//     <Body bone="LeftAnkle" swingFrom="LeftThigh" cone="35" plane="-20 20" twist="-10 10"/>
//   </Ragdoll>
enum class Source : std::uint8_t { kAction, kBuiltIn };
struct Profile {
    CarryPartialRagdoll::Dynamics motion;
    std::vector<CarryPartialRagdoll::BodyTuning> bodies;
    std::string source;  // file path or "built-in", for logs
    Source from = Source::kBuiltIn;
};
// Under each mod folder. The declaration scan reads only a mod folder's own files, so it never reads these.
inline constexpr std::wstring_view kFolder = L"Ragdoll";
// Strict: an unknown element or attribute, a value out of range or a repeated bone rejects the
// whole file with the reason in `error`. `rig` empty accepts any rig name.
std::optional<Profile> Parse(const std::filesystem::path& file, std::string_view rig, std::string& error);
// <folder of actionXml>/Ragdoll/<liftId>.xml. Empty when liftId cannot name a file
// (empty, ".", "..", a path separator or ':').
std::filesystem::path ActionProfilePath(const std::filesystem::path& actionXml, std::string_view liftId);
// The built-in values, vanilla for every rig: the game's own ragdoll values (native damping, gravity, inertia and
// joints, no follow share, every body simulating).
Profile BuiltIn(PairedRig::Kind rig);
// The lift action's file when it parses for `rig`, else BuiltIn(rig) (logged). Main thread.
Profile Load(PairedRig::Kind rig, const std::filesystem::path& actionXml, std::string_view liftId);
}
