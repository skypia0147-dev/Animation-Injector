#include "PCH.h"
#include "runtime/HitHistory.h"
#include <mutex>

namespace AnimationInjector::HitHistory {
namespace {
struct Entry {
    RE::ActorHandle actor;
    Hit hit;
    float at = 0.0f;  // the clock when it was hit
};
bool needed = false;
// Conditions read it from the graph notify hook too (a start's vanilla block preflight), off the main thread.
std::mutex lock;
float clock = 0.0f;
// One entry per actor hit since the load; the oldest goes past this many.
constexpr std::size_t kMaxActors = 256;
std::vector<Entry> entries;
}  // namespace

bool Matches(const Hit& hit, std::string_view option) noexcept {
    if (option == "normal") return !hit.power && !hit.bash;
    if (option == "power") return hit.power;
    if (option == "bash") return hit.bash;
    if (option == "sneak") return hit.sneak;
    if (option == "blocked") return hit.blocked;
    return false;
}

void SetNeeded(bool value) {
    needed = value;
    if (!needed) ResetOnLoad();
}
bool Needed() noexcept { return needed; }

void Advance(float delta) {
    if (!needed || !std::isfinite(delta) || delta <= 0.0f || delta >= 1.0f) return;
    std::lock_guard guard(lock);
    clock += delta;
}

void Record(RE::ActorHandle actor, const Hit& hit) {
    if (!needed || !actor) return;
    std::lock_guard guard(lock);
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.actor == actor; });
    if (found != entries.end()) {
        *found = {actor, hit, clock};
        return;
    }
    if (entries.size() >= kMaxActors) entries.erase(entries.begin());
    entries.push_back({actor, hit, clock});
}

bool LastOf(RE::ActorHandle actor, Hit& hit, float& ago) {
    std::lock_guard guard(lock);
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.actor == actor; });
    if (found == entries.end()) return false;
    hit = found->hit;
    ago = clock - found->at;
    return true;
}

void ResetOnLoad() {
    std::lock_guard guard(lock);
    entries.clear();
    clock = 0.0f;
}

}  // namespace AnimationInjector::HitHistory
