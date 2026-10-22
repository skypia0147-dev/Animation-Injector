#include "runtime/AnnotationCommands.h"
#include "runtime/EventEffects.h"
#include <atomic>
#include <chrono>
#include <mutex>

namespace AnimationInjector::AnnotationCommands {
namespace {
struct Queued {
    RE::ActorHandle actor;
    Parsed command;
    std::uint64_t epoch = 0;
};
std::mutex queueMutex;
std::vector<Queued> queue;
std::atomic<std::uint64_t> epoch{0};
constexpr std::size_t kQueueLimit = 64;

std::mutex reportedMutex;
std::vector<std::string> reported;  // invalid payloads already logged
constexpr std::size_t kReportLimit = 32;
PairEcho echo;  // main thread
double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}

void Observe(RE::Actor* actor, const RE::BSFixedString& payload) {
    if (!actor) return;
    const std::string_view text = payload.c_str() ? payload.c_str() : "";
    const auto parsed = Parse(text);
    if (!parsed) {
        std::lock_guard lock(reportedMutex);
        if (reported.size() < kReportLimit && std::find(reported.begin(), reported.end(), text) == reported.end()) {
            reported.emplace_back(text);
            logger::warn("[AnnotationCommands] 'AI.{}' is not a command (AI.CamShake|strength|duration, every "
                         "number written)",
                text);
        }
        return;
    }
    std::lock_guard lock(queueMutex);
    if (queue.size() < kQueueLimit) queue.push_back({actor->GetHandle(), *parsed, epoch.load()});
}

void Update() {
    std::vector<Queued> arrived;
    {
        std::lock_guard lock(queueMutex);
        if (queue.empty()) return;
        arrived.swap(queue);
    }
    const auto now = epoch.load();
    for (const auto& q : arrived) {
        if (q.epoch != now) continue;
        auto actor = q.actor.get();
        if (!actor || !actor->Is3DLoaded()) continue;
        const auto context = EventEffects::ContextFor(actor.get(), "AI");
        const auto partner = context.partner.get();
        if (!context.senderIsPlayer && !(partner && partner->IsPlayerRef())) continue;
        if (echo.Repeat(q.actor.native_handle(), context.partner.native_handle(), q.command, Now())) {
            logger::debug("[AnnotationCommands] the pair's other role sent the same command: run once");
            continue;
        }
        switch (q.command.command) {
        case Command::kCamShake:
            RE::ShakeCamera(q.command.strength, actor->GetPosition(), q.command.duration);
            logger::debug("[AnnotationCommands] CamShake {:.2f} for {:.2f}s from {:08X}", q.command.strength,
                q.command.duration, actor->GetFormID());
            break;
        }
    }
}

void ResetOnLoad() {
    ++epoch;
    echo.Clear();
    std::lock_guard lock(queueMutex);
    queue.clear();
}
}
