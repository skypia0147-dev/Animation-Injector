#pragma once

namespace AnimationInjector::AnimationLoadObserver {
// Observe native loader results without changing acquisition or playback.
void Install();
void ResetProgress();
// The loading session stopped waiting (core/LoadingProgressModel.h kStallLimit): logs the engine requests it waited on.
void ReportPending();
}
