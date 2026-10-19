#pragma once

// Names of the shared full-body entry (core/FullBodyEntry.h), without engine headers.
namespace AnimationInjector::FullBodyEntry {
inline constexpr const char* kStateVariable = "iAI_FullBodyState";
inline constexpr const char* kBlendVariable = "fAI_FullBodyBlend";
inline constexpr const char* kEnterEvent = "AI_FullBodyEnter";
inline constexpr const char* kLinkEnterEvent = "AI_FullBodyLinkEnter";
}
