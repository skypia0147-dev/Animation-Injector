#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace AnimationInjector::LoadingProgress {
using Token = std::uint64_t;
enum class Phase { kRegistration, kAnimation };
enum class Outcome { kReady, kFailed, kCancelled };
enum class Session { kStartup, kSaveLoading, kInitialGameplay, kClosed };

// How full one of the engine's limits is (shown under the progress): the fullest graph's node ids, the fullest
// character project's animation table. Known once something reported it; kept for the whole game session.
struct Usage {
    std::size_t used = 0, limit = 0;
    bool shortfall = false;  // something was left out because it would not fit
    bool Known() const { return limit > 0; }
    bool Warn() const { return Known() && used * 10 >= limit * 9; }
    float Fraction() const {
        return Known() ? std::min(1.0f, static_cast<float>(used) / static_cast<float>(limit)) : 0;
    }
};

struct Snapshot {
    std::uint64_t revision = 0;
    std::size_t total = 0, ready = 0, failed = 0, cancelled = 0, pending = 0;
    Phase phase = Phase::kRegistration;
    bool visible = false;
    float alpha = 0;
    Usage nodes, animations;
    float Fraction() const {
        const auto count = total - cancelled;
        return count ? static_cast<float>(ready) / static_cast<float>(count) : 0;
    }
};

// Pure accounting. The service serializes producers and the UI consumer.
// One display batch per startup/save load. Completion is retained until a UI
// frame observes it, then the session closes instead of reappearing in gameplay.
class Model {
public:
    Token Begin(Phase phase) {
        if (session == Session::kClosed) return 0;
        if (!snapshot.visible) Clear(snapshot.revision);
        const Token token = ++sequence;
        jobs.emplace(token, phase);
        ++snapshot.total;
        snapshot.pending = jobs.size();
        snapshot.phase = phase;
        snapshot.visible = true;
        snapshot.alpha = 1;
        ++snapshot.revision;
        completionSeen = false;
        elapsed = 0;
        return token;
    }
    bool ObservesNativeRequests() const { return session == Session::kStartup || session == Session::kSaveLoading; }
    bool IsOpen() const { return session != Session::kClosed; }
    Token BeginNativeAnimation() { return ObservesNativeRequests() ? Begin(Phase::kAnimation) : 0; }
    bool Finish(Token token, Outcome result) {
        if (!token || !jobs.erase(token)) return false;
        switch (result) {
        case Outcome::kReady: ++snapshot.ready; break;
        case Outcome::kFailed: ++snapshot.failed; break;
        case Outcome::kCancelled: ++snapshot.cancelled; break;
        }
        snapshot.pending = jobs.size();
        if (!jobs.empty()) snapshot.phase = jobs.begin()->second;
        ++snapshot.revision;
        return true;
    }
    Snapshot Read() const { return snapshot; }
    // The fuller report wins (third and first person, male and female projects report separately).
    void ReportNodes(std::size_t used, std::size_t limit, bool shortfall) {
        Report(snapshot.nodes, used, limit, shortfall);
    }
    void ReportAnimations(std::size_t used, std::size_t limit, bool shortfall) {
        Report(snapshot.animations, used, limit, shortfall);
    }
    Snapshot Advance(float delta, bool canComplete = true) {
        if (session == Session::kClosed || session == Session::kSaveLoading || !canComplete || snapshot.pending)
            return snapshot;
        if (!completionSeen) {
            completionSeen = true;
            return snapshot;
        }
        if (std::isfinite(delta) && delta > 0) elapsed += std::min(delta, 0.1f);
        // A limit that left something out stays up long enough to be read; one close to full a little longer.
        const bool shortfall = snapshot.nodes.shortfall || snapshot.animations.shortfall;
        const bool warn = snapshot.nodes.Warn() || snapshot.animations.Warn();
        const float hold = shortfall ? 8.0f : snapshot.failed || warn ? 3.0f : 0.7f;
        snapshot.alpha = std::clamp(1.0f - (elapsed - hold) / 0.25f, 0.0f, 1.0f);
        if (snapshot.alpha <= 0) {
            snapshot.visible = false;
            session = Session::kClosed;
            ++snapshot.revision;
        }
        return snapshot;
    }
    void Reset(bool waitingForGameLoad = false) {
        jobs.clear();
        Clear(snapshot.revision + 1);
        completionSeen = false;
        elapsed = 0;
        session = waitingForGameLoad ? Session::kSaveLoading : Session::kStartup;
        // Never reuse a token: late callbacks from an old save must be ignored.
    }
    void GameLoaded() {
        // Only PreLoadGame/NewGame may reopen a closed session. PostLoadGame
        // releases the completion fence and stops admitting gameplay requests.
        if (session == Session::kClosed) return;
        session = Session::kInitialGameplay;
        completionSeen = false;
        elapsed = 0;
    }

private:
    // A new batch starts from zero; the usage stays (the graphs are built once per game session).
    void Clear(std::uint64_t revision) {
        const auto nodes = snapshot.nodes, animations = snapshot.animations;
        snapshot = {};
        snapshot.revision = revision;
        snapshot.nodes = nodes;
        snapshot.animations = animations;
    }
    void Report(Usage& usage, std::size_t used, std::size_t limit, bool shortfall) {
        if (!limit) return;
        Usage next = usage;
        if (!usage.Known() || used * usage.limit >= usage.used * limit) {
            next.used = used;
            next.limit = limit;
        }
        next.shortfall = usage.shortfall || shortfall;
        if (next.used == usage.used && next.limit == usage.limit && next.shortfall == usage.shortfall) return;
        usage = next;
        ++snapshot.revision;
    }
    Token sequence = 0;
    Snapshot snapshot;
    std::unordered_map<Token, Phase> jobs;
    bool completionSeen = false;
    float elapsed = 0;
    Session session = Session::kStartup;
};
}
