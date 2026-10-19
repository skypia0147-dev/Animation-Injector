#pragma once
#include <cstdint>

namespace AnimationInjector::CarrySyncResolver {
// The reader must supply complete, readable native function ranges. Nothing is
// executed by this resolver; unsupported or modified call chains fail closed.
using CodeRange = bool (*)(std::uintptr_t start, std::uintptr_t& end);
std::uintptr_t Resolve(std::uintptr_t mountUpdate, std::uintptr_t syncVtable, CodeRange range);
}
