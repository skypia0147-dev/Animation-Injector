#pragma once
#include <string>
#include <vector>
// Test double: the files of the own locomotion trees (the real module reads the declaration and the game's files).
namespace AnimationInjector::OwnLocomotion {
inline std::vector<std::string> treeFiles;
inline const std::vector<std::string>& Files() { return treeFiles; }
}
