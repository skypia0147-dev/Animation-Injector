#include "PCH.h"
#include "core/AnimationLoadObserver.h"
#include "core/LoadingProgress.h"
#include "core/NodeFactory.h"
#include "runtime/ActionRegistry.h"
#include "RE/A/AnimationFileManagerSingleton.h"
#include <atomic>
#include <mutex>

namespace AnimationInjector::AnimationLoadObserver {
namespace {
// Numeric identities are compared only, never dereferenced after a callback.
struct Key {
    std::uintptr_t clip = 0, character = 0;
    bool operator==(const Key&) const = default;
};
struct KeyHash {
    std::size_t operator()(const Key& key) const {
        return std::hash<std::uintptr_t>{}(key.clip) ^ (std::hash<std::uintptr_t>{}(key.character) << 1);
    }
};
struct Request {
    std::uint64_t serial = 0;
    LoadingProgress::Token progress = 0;
    bool queueInFlight = true, loaded = false;
};
struct Ticket {
    Key key;
    std::uint64_t serial = 0;
};
std::unordered_map<Key, Request, KeyHash> requests;
std::mutex mutex;
std::uint64_t sequence = 0;
std::atomic_bool hasRequests = false;

Key Identity(const RE::hkbContext& context, RE::hkbClipGenerator* clip) {
    return {reinterpret_cast<std::uintptr_t>(clip), reinterpret_cast<std::uintptr_t>(context.character)};
}
bool Eligible(const RE::hkbContext& context, RE::hkbClipGenerator* clip, RE::BSSynchronizedClipGenerator* sync) {
    if (!LoadingProgress::ObservesNativeRequests()) return false;
    // OAR also passes lightweight, non-hkb objects to this interface. Do not
    // read their names or controls. Paired preloaders already own their UI jobs.
    if (sync || !context.character || !NodeFactory::SameVTable(clip, RE::VTABLE_hkbClipGenerator)) return false;
    int action = -1, variant = -1;
    return Actions::FindClip(clip->name.c_str(), action, variant) && !Actions::Spec(action).paired.enabled;
}
Ticket Reserve(Key key) {
    std::lock_guard lock(mutex);
    auto [it, added] = requests.try_emplace(key);
    if (added) it->second.serial = ++sequence;
    hasRequests.store(true, std::memory_order_relaxed);
    return {key, it->second.serial};
}
Ticket Find(Key key) {
    if (!hasRequests.load(std::memory_order_relaxed)) return {};
    std::lock_guard lock(mutex);
    const auto it = requests.find(key);
    return it == requests.end() ? Ticket{} : Ticket{key, it->second.serial};
}
void Erase(decltype(requests)::iterator it, LoadingProgress::Outcome outcome) {
    LoadingProgress::Finish(it->second.progress, outcome);
    requests.erase(it);
    hasRequests.store(!requests.empty(), std::memory_order_relaxed);
}
void Queued(Ticket ticket, std::int32_t result) {
    if (!ticket.serial) return;
    std::lock_guard lock(mutex);
    const auto it = requests.find(ticket.key);
    if (it == requests.end() || it->second.serial != ticket.serial) return;
    if (result != 0) {
        // Queue=1 acquired a resident animation: no new loading task.
        // Other results are not evidence of an outstanding request.
        Erase(it, result == 1 ? LoadingProgress::Outcome::kReady : LoadingProgress::Outcome::kCancelled);
        return;
    }
    auto& request = it->second;
    request.queueInFlight = false;
    if (!request.progress) {
        request.progress = LoadingProgress::BeginNativeAnimation();
        // The load boundary can change while the native Queue is executing.
        if (!request.progress) {
            Erase(it, LoadingProgress::Outcome::kCancelled);
            return;
        }
        logger::trace("[Loading] native queue request={} clip={:X} character={:X}", ticket.serial, ticket.key.clip,
            ticket.key.character);
    }
    if (request.loaded) Erase(it, LoadingProgress::Outcome::kReady);
}
void Loaded(Ticket ticket, bool result) {
    if (!ticket.serial || !result) return;
    std::lock_guard lock(mutex);
    const auto it = requests.find(ticket.key);
    if (it == requests.end() || it->second.serial != ticket.serial) return;
    // Load may complete before Queue returns. Publish its result after the
    // queue result is known; even a genuinely queued, fast load is displayed.
    if (it->second.queueInFlight) {
        it->second.loaded = true;
        return;
    }
    logger::trace("[Loading] native ready request={} clip={:X} character={:X}", ticket.serial, ticket.key.clip,
        ticket.key.character);
    Erase(it, LoadingProgress::Outcome::kReady);
}
void Cancel(Key key) {
    if (!hasRequests.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(mutex);
    const auto it = requests.find(key);
    if (it != requests.end()) Erase(it, LoadingProgress::Outcome::kCancelled);
}

struct Queue {
    static std::int32_t thunk(RE::AnimationFileManagerSingleton* manager, const RE::hkbContext& context,
        RE::hkbClipGenerator* clip, RE::BSSynchronizedClipGenerator* sync) {
        const auto ticket = Eligible(context, clip, sync) ? Reserve(Identity(context, clip)) : Ticket{};
        // Never hold the observer mutex across an engine/other-plugin call.
        const auto result = func(manager, context, clip, sync);
        Queued(ticket, result);
        return result;
    }
    static inline REL::Relocation<decltype(thunk)> func;
};
struct Load {
    static bool thunk(RE::AnimationFileManagerSingleton* manager, const RE::hkbContext& context,
        RE::hkbClipGenerator* clip, RE::BSSynchronizedClipGenerator* sync) {
        const auto ticket = Find(Identity(context, clip));
        const bool result = func(manager, context, clip, sync);
        Loaded(ticket, result);
        return result;
    }
    static inline REL::Relocation<decltype(thunk)> func;
};
struct Unload {
    static std::int32_t thunk(RE::AnimationFileManagerSingleton* manager, const RE::hkbContext& context,
        RE::hkbClipGenerator* clip, RE::BSSynchronizedClipGenerator* sync) {
        // Retire the old identity before native teardown can reuse its address.
        Cancel(Identity(context, clip));
        return func(manager, context, clip, sync);
    }
    static inline REL::Relocation<decltype(thunk)> func;
};
}

void ResetProgress() {
    std::lock_guard lock(mutex);
    for (auto& [key, request] : requests)
        LoadingProgress::Finish(request.progress, LoadingProgress::Outcome::kCancelled);
    requests.clear();
    hasRequests.store(false, std::memory_order_relaxed);
    // Keep serials monotonic to reject callbacks that straddle a save reload.
}
void Install() {
    REL::Relocation<std::uintptr_t> vtable{RE::VTABLE_AnimationFileManagerSingleton[0]};
    Queue::func = vtable.write_vfunc(0x1, Queue::thunk);
    Load::func = vtable.write_vfunc(0x2, Load::thunk);
    Unload::func = vtable.write_vfunc(0x3, Unload::thunk);
    logger::info("[Loading] native Queue/Load/Unload observers installed");
}
}
