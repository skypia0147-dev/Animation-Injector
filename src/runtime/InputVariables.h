#pragma once

// Registered graph variables the runtime keeps (1.1.0): <VariableRegistry><Variable ... source="..."/>.
//
//   source="moveInput" (int)   the player's movement input, as the keys or the left stick give it
//                              (InputSink::CurrentMoveDir): 0 none, 1 forward, 2 right, 3 back, 4 left.
//                              The raw input: True Directional Movement's turning does not change it.
//
//   source="strafeInput" (int) the same value while the keys move the character sideways and back, and 0 while
//                              they turn it instead: True Directional Movement's directional setting, without a
//                              target lock. A moveset's side and back attacks belong to the first case only.
//                              An OAR condition on TDM's own global cannot tell this inside an action: TDM sees
//                              no movement input there and reports directional whatever the setting.
//
// An Open Animation Replacer condition reads it by name (CompareValues, graphVariable of type Int), so a moveset
// picks its directional attacks without a key tracking plugin. Only the player has input: every other actor's
// variable stays at its initial 0.
//
// Written when the value changes, and again after a load (the new graph starts at 0). A write the active graph
// refuses (the player in first person: the variables are in the third-person graphs) is tried again each frame.
namespace AnimationInjector::InputVariables {

// Main thread, once a frame (the player's update).
void Update();
// Loads: what was written is forgotten, so the next Update writes again.
void Reset();

}  // namespace AnimationInjector::InputVariables
