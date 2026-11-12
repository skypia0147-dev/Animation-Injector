#pragma once
namespace AnimationInjector::InputDispatchHook {
bool InstallIfNeeded();
// Load / new game: an activate press kept from the game (a horse or furniture during a player action) whose release
// the old session never saw is forgotten.
void Reset();
}
