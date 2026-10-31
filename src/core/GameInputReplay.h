#pragma once
#include "PCH.h"

namespace AnimationInjector::GameInputReplay {
// A game control's press handed to the game late (10-30, runtime/PressKinds.h): a start waited to tell a tap from a
// hold or a double, and the press turned out to be the game's. Sent through the input device manager like a real
// press, so every handler sees it (the game's controls and other mods'). Main thread.
// `tap`: down and up together (the key is already up); otherwise down only, and the real release follows.
void Send(RE::INPUT_DEVICE device, std::uint32_t idCode, const RE::BSFixedString& userEvent, bool tap);
// While Send runs: the sink and the dispatch hook pass the press through untouched.
bool Replaying();
}  // namespace AnimationInjector::GameInputReplay
