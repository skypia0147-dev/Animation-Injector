#pragma once

namespace AnimationInjector::AnimationLoadObserver {
// Observe native loader results without changing acquisition or playback.
void Install();
void ResetProgress();
}
