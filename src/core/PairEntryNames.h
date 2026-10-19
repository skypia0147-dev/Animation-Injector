#pragma once

// Names of the shared pair entry (core/PairEntry.h), without engine headers.
namespace AnimationInjector::PairEntry {
// Root states that hold the paired states of one role, and their nested machines.
inline constexpr const char* kLeadContainer = "AI_PairLead";
inline constexpr const char* kSupportContainer = "AI_PairSupport";
inline constexpr const char* kLeadMachine = "AI_PairLead_Behavior";
inline constexpr const char* kSupportMachine = "AI_PairSupport_Behavior";
// Shared root entries, one per role.
inline constexpr const char* kEnterLeadEvent = "AI_PairEnterLead";
inline constexpr const char* kEnterSupportEvent = "AI_PairEnterSupport";
// Per-actor graph variables set right before a shared entry.
inline constexpr const char* kStateVariable = "iAI_PairState";
inline constexpr const char* kExitBlendVariable = "fAI_PairExitBlend";
inline constexpr const char* kLossBlendVariable = "fAI_PairLossBlend";
inline constexpr const char* kRecoveryBlendVariable = "fAI_PairRecoveryBlend";
}
