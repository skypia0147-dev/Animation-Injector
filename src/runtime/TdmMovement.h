#pragma once

namespace AnimationInjector::TdmMovement {
// The player's movement direction as the character moves (10-30). True Directional Movement, when installed, has a
// directional mode in which the player turns toward the movement input and always runs forward: there a movement
// condition reads forward whatever key is held. Its other modes (off, facing the crosshair, target lock) move the
// player the way the keys point, as without TDM. Optional: without TDM, or with a TDM too old to report its mode
// (API v3), the keys decide as they always did.

// What TDM says now (TDM_API::DirectionalMovementMode), or kAbsent.
enum class Mode { kAbsent, kDisabled, kVanillaStyle, kDirectional, kTargetLock };

// The direction a movement condition reads, from the keys' direction (MoveDir: 0 forward, 1 back, 2 left, 3 right,
// 4 none; -1 unknown).
inline int ForCharacter(int keys, Mode mode) {
    constexpr int kForward = 0, kNone = 4;
    return mode == Mode::kDirectional && keys >= 0 && keys != kNone ? kForward : keys;
}

// Finds TDM once the game data is in (kDataLoaded): every plugin is loaded by then.
void Load();
// TDM's mode now; kAbsent before Load or without TDM v3.
Mode Current();
}  // namespace AnimationInjector::TdmMovement
