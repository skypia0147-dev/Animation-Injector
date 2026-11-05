#pragma once
// Test double: the vanilla signed limit (the real module reads the engine and OAR's INI).
namespace AnimationInjector::AnimationLimit {
inline int MaxBindingIndex() { return 0x7FFF; }
inline int NameTableLimit() { return 0x7FFF - 1024; }
}
