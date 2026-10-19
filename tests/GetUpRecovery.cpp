#include "runtime/GetUpRecovery.h"
#include <cstdlib>
#include <iostream>

using namespace AnimationInjector::GetUpRecovery;
namespace {
unsigned checks = 0;
void Check(bool condition, int line) {
    ++checks;
    if (!condition) {
        std::cerr << "Failed check at line " << line << '\n';
        std::exit(1);
    }
}
#define CHECK(x) Check((x), __LINE__)
Sample Knock(int knock, bool held = false) { return {true, false, held, knock}; }
// Runs frames of 10 ms until a step other than kNone or the time runs out.
Step Run(Watch& watch, const Sample& sample, float seconds, float* at = nullptr) {
    float t = 0.0f;
    while (t < seconds) {
        t += 0.01f;
        const auto step = Advance(watch, sample, 0.01f);
        if (step != Step::kNone) {
            if (at) *at = t;
            return step;
        }
    }
    if (at) *at = t;
    return Step::kNone;
}
}

int main() {
    {  // Native path (0.12.153 #1): explode, rest, GetUpStart 0.51 s later. Never interfere.
        Watch w;
        CHECK(Run(w, Knock(1), 1.1f) == Step::kNone);
        CHECK(Run(w, Knock(kKnockQueued), 0.51f) == Step::kNone);
        CHECK(Advance(w, Knock(kKnockGetUp), 0.01f) == Step::kRecovered && w.attempts == 0);
    }
    {  // Lost request (0.12.154 #3): rests at 0.07 s and never leaves kQueued.
        Watch w;
        float at = 0.0f;
        CHECK(Run(w, Knock(kKnockQueued), 5.0f, &at) == Step::kRequestGetUp);
        CHECK(at > 0.99f && at < 1.02f && w.attempts == 1);
        CHECK(Run(w, Knock(kKnockQueued), 0.6f) == Step::kNone);  // Graph timer runs 0.5 s.
        CHECK(Advance(w, Knock(kKnockGetUp), 0.01f) == Step::kRecovered);
    }
    {  // Graph not in FullyRagdoll: escalate to RagdollInstant, then GetUpBegin after it settles.
        Watch w;
        CHECK(Run(w, Knock(kKnockQueued), 5.0f) == Step::kRequestGetUp);
        float at = 0.0f;
        CHECK(Run(w, Knock(kKnockQueued), 5.0f, &at) == Step::kEnterRagdoll && at > 0.99f && at < 1.02f);
        CHECK(Run(w, Knock(kKnockQueued), 1.0f, &at) == Step::kRequestGetUp && at > 0.09f && at < 0.12f);
        CHECK(Run(w, Knock(kKnockQueued), 5.0f) == Step::kEnterRagdoll);
        CHECK(Run(w, Knock(kKnockQueued), 1.0f) == Step::kRequestGetUp && w.attempts == 3);
        CHECK(Run(w, Knock(kKnockQueued), 5.0f) == Step::kGiveUp);
    }
    {  // The settle delay never requests a get-up once the engine left kQueued by itself.
        Watch w;
        CHECK(Run(w, Knock(kKnockQueued), 5.0f) == Step::kRequestGetUp);
        CHECK(Run(w, Knock(kKnockQueued), 5.0f) == Step::kEnterRagdoll);
        CHECK(Run(w, Knock(1), 0.2f) == Step::kNone && w.settle < 0.0f);
    }
    {  // A long flight (kExplode) or a held state is not a stall; held time does not accumulate.
        Watch w;
        CHECK(Run(w, Knock(1), 4.0f) == Step::kNone);
        CHECK(Run(w, Knock(kKnockQueued, true), 4.0f) == Step::kNone);
        CHECK(Run(w, Knock(kKnockQueued), 0.9f) == Step::kNone && w.attempts == 0);
    }
    {  // Death, unloading and the watch lifetime end the watch without an event.
        Watch w;
        CHECK(Advance(w, {true, true, false, kKnockQueued}, 0.01f) == Step::kGone);
        CHECK(Advance(w, {}, 0.01f) == Step::kGone);
        CHECK(Run(w, Knock(1), kWatchSeconds + 1.0f) == Step::kGiveUp);
        Watch bad;
        CHECK(Advance(bad, Knock(kKnockQueued), -1.0f) == Step::kNone && bad.life == 0.0f);
    }
    std::cout << "GetUpRecovery: native path, lost request, escalation, holds and lifetime passed (" << checks
              << " checks)\n";
}
