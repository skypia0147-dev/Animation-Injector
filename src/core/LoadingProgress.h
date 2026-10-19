#pragma once
#include "core/LoadingProgressModel.h"

namespace AnimationInjector::LoadingProgress {
void SetEnabled(bool enabled);
bool IsActive();
Token Begin(Phase phase);
bool ObservesNativeRequests();
Token BeginNativeAnimation();
void Finish(Token& token, Outcome result);
Snapshot Read();
Snapshot Advance(float delta, bool canComplete = true);
void Reset(bool waitingForGameLoad = false);
void GameLoaded();

class Registration {
public:
    explicit Registration(Outcome initial = Outcome::kReady) : token(Begin(Phase::kRegistration)), result(initial) {}
    Registration(const Registration&) = delete;
    Registration& operator=(const Registration&) = delete;
    ~Registration() { Finish(token, result); }
    void Fail() { result = Outcome::kFailed; }
    void Ready() { result = Outcome::kReady; }

private:
    Token token;
    Outcome result = Outcome::kReady;
};
}
