#include "PCH.h"
#include "runtime/PayloadCommands.h"
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace AnimationInjector::PayloadCommands {
namespace {
struct Queued {
    RE::ActorHandle actor;
    RE::BSFixedString payload;  // a counted reference: no text is copied in the sink
    std::uint64_t epoch = 0;
};
std::mutex queueMutex;
std::vector<Queued> queue;
std::atomic<std::uint64_t> epoch{0};
constexpr std::size_t kQueueLimit = 256;

// A payload's command, read once (main thread). The key holds the string, so its pointer is not reused while
// the entry lives. A payload that is no command of ours (a macro, another command) is remembered as none.
struct Known {
    bool command = false;
    Parsed parsed;
    RE::BSFixedString variable;
};
struct Hash {
    std::size_t operator()(const RE::BSFixedString& s) const noexcept { return std::hash<const void*>{}(s.data()); }
};
std::unordered_map<RE::BSFixedString, Known, Hash> known;
constexpr std::size_t kKnownLimit = 8192;

// Whether Payload Interpreter is in the process: decided at the first "PIE" event, long after every plugin loaded.
bool OwnerPresent() {
    static const bool present = [] {
        const bool there = ::GetModuleHandleW(L"PayloadInterpreter.dll") != nullptr;
        logger::info("[PayloadCommands] Payload Interpreter {}: @SGVI, @SGVF and @CAMSHAKE are {}",
            there ? "is installed" : "is not installed", there ? "left to it" : "run by the runtime");
        return there;
    }();
    return present;
}
}  // namespace

void Observe(RE::Actor* actor, const RE::BSFixedString& payload) {
    if (!actor || payload.empty() || OwnerPresent()) return;
    std::lock_guard lock(queueMutex);
    if (queue.size() < kQueueLimit) queue.push_back({actor->GetHandle(), payload, epoch.load()});
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
        auto it = known.find(q.payload);
        if (it == known.end()) {
            if (known.size() >= kKnownLimit) known.clear();
            Known entry;
            if (const auto parsed = Parse(std::string_view(q.payload.data(), q.payload.size()))) {
                entry.command = true;
                entry.parsed = *parsed;
                if (parsed->kind != Kind::kCamShake) entry.variable = RE::BSFixedString(parsed->variable.c_str());
            }
            it = known.emplace(q.payload, std::move(entry)).first;
        }
        const auto& k = it->second;
        if (!k.command) continue;
        const auto actor = q.actor.get();
        if (!actor || !actor->Is3DLoaded()) continue;
        switch (k.parsed.kind) {
        case Kind::kSetInt: actor->SetGraphVariableInt(k.variable, k.parsed.integer); break;
        case Kind::kSetFloat: actor->SetGraphVariableFloat(k.variable, k.parsed.number); break;
        case Kind::kCamShake: RE::ShakeCamera(k.parsed.number, actor->GetPosition(), k.parsed.seconds); break;
        }
    }
}

void ResetOnLoad() {
    ++epoch;
    std::lock_guard lock(queueMutex);
    queue.clear();
}
}  // namespace AnimationInjector::PayloadCommands
