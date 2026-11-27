#pragma once

// The preloader tests declare no optional action: every clip is installed.
namespace AnimationInjector::ClipPresence {
inline bool Absent(int) { return false; }
}
