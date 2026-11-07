#include "PCH.h"
#include "runtime/EventHistory.h"
#include <atomic>
#include <mutex>

namespace AnimationInjector::EventHistory {
namespace {
struct Entry {
    RE::ActorHandle actor;
    std::vector<float> at;  // per name id: the clock when it last arrived, < 0 = not yet
};
std::mutex lock;
std::atomic_bool needed = false;
std::vector<std::string> names;  // by id
float clock = 0.0f;
// One entry per actor that received an asked event since the load; the oldest goes past this many.
constexpr std::size_t kMaxActors = 256;
std::vector<Entry> entries;
int IdOf(const char* event) {
    for (std::size_t i = 0; i < names.size(); ++i)
        if (_stricmp(names[i].c_str(), event) == 0) return static_cast<int>(i);
    return -1;
}
}  // namespace

std::vector<int> SetNames(const std::vector<std::string>& asked) {
    std::lock_guard guard(lock);
    names.clear();
    entries.clear();
    clock = 0.0f;
    std::vector<int> ids;
    for (const auto& name : asked) {
        int id = IdOf(name.c_str());
        if (id < 0) {
            id = static_cast<int>(names.size());
            names.push_back(name);
        }
        ids.push_back(id);
    }
    needed.store(!names.empty());
    return ids;
}
bool Needed() noexcept { return needed.load(std::memory_order_relaxed); }

void Advance(float delta) {
    if (!Needed() || !std::isfinite(delta) || delta <= 0.0f || delta >= 1.0f) return;
    std::lock_guard guard(lock);
    clock += delta;
}

void Record(RE::Actor* actor, const char* event) {
    if (!Needed() || !actor || !event || !*event) return;
    std::lock_guard guard(lock);
    const int id = IdOf(event);
    if (id < 0) return;
    const auto handle = actor->GetHandle();
    auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.actor == handle; });
    if (found == entries.end()) {
        if (entries.size() >= kMaxActors) entries.erase(entries.begin());
        entries.push_back({handle, std::vector<float>(names.size(), -1.0f)});
        found = std::prev(entries.end());
    }
    found->at[static_cast<std::size_t>(id)] = clock;
}

bool Ago(RE::ActorHandle actor, int id, float& ago) {
    std::lock_guard guard(lock);
    if (id < 0 || static_cast<std::size_t>(id) >= names.size()) return false;
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.actor == actor; });
    if (found == entries.end() || found->at[static_cast<std::size_t>(id)] < 0.0f) return false;
    ago = clock - found->at[static_cast<std::size_t>(id)];
    return true;
}

void ResetOnLoad() {
    std::lock_guard guard(lock);
    entries.clear();
    clock = 0.0f;
}

}  // namespace AnimationInjector::EventHistory
