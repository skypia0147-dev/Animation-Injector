#include "core/LoadingProgress.h"
#include <mutex>
#include <atomic>

namespace AnimationInjector::LoadingProgress {
namespace {
Model model;
std::mutex mutex;
std::atomic_bool observesNativeRequests = true;
std::atomic_bool active = true;
bool enabled = true;
}
void SetEnabled(bool value) {
    std::lock_guard lock(mutex);
    enabled = value;
    model.Reset();
    active.store(value, std::memory_order_relaxed);
    observesNativeRequests.store(value, std::memory_order_relaxed);
}
bool IsActive() { return active.load(std::memory_order_relaxed); }
Token Begin(Phase phase) {
    if (!IsActive()) return 0;
    std::lock_guard lock(mutex);
    return enabled ? model.Begin(phase) : 0;
}
bool ObservesNativeRequests() { return observesNativeRequests.load(std::memory_order_relaxed); }
Token BeginNativeAnimation() {
    if (!ObservesNativeRequests()) return 0;
    std::lock_guard lock(mutex);
    return enabled ? model.BeginNativeAnimation() : 0;
}
void Finish(Token& token, Outcome result) {
    if (!token) return;
    std::lock_guard lock(mutex);
    model.Finish(token, result);
    token = 0;
}
Snapshot Read() {
    std::lock_guard lock(mutex);
    return model.Read();
}
Snapshot Advance(float delta, bool canComplete) {
    std::lock_guard lock(mutex);
    const auto state = model.Advance(delta, canComplete);
    observesNativeRequests.store(enabled && model.ObservesNativeRequests(), std::memory_order_relaxed);
    active.store(enabled && model.IsOpen(), std::memory_order_relaxed);
    return state;
}
void Reset(bool waitingForGameLoad) {
    std::lock_guard lock(mutex);
    model.Reset(waitingForGameLoad);
    observesNativeRequests.store(enabled, std::memory_order_relaxed);
    active.store(enabled, std::memory_order_relaxed);
}
void GameLoaded() {
    std::lock_guard lock(mutex);
    model.GameLoaded();
    observesNativeRequests.store(enabled && model.ObservesNativeRequests(), std::memory_order_relaxed);
}
}
