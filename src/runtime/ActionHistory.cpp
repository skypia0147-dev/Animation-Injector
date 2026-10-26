#include "PCH.h"
#include "runtime/ActionHistory.h"

namespace AnimationInjector::ActionHistory {
namespace {
struct Entry {
    RE::ActorHandle actor;
    int index = -1;
    float at = 0.0f;  // the clock when it finished
    bool completed = false;
};
bool needed = false;
float clock = 0.0f;
// One entry per actor that finished one of our actions since the load; the oldest goes past this many.
constexpr std::size_t kMaxActors = 256;
std::vector<Entry> entries;
}  // namespace

void SetNeeded(bool value) {
    needed = value;
    if (!needed) ResetOnLoad();
}
bool Needed() noexcept { return needed; }

void Advance(float delta) {
    if (needed && std::isfinite(delta) && delta > 0.0f && delta < 1.0f) clock += delta;
}

void Record(RE::ActorHandle actor, int index, bool completed) {
    if (!needed || !actor) return;
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.actor == actor; });
    if (found != entries.end()) {
        *found = {actor, index, clock, completed};
        return;
    }
    if (entries.size() >= kMaxActors) entries.erase(entries.begin());
    entries.push_back({actor, index, clock, completed});
}

bool LastOf(RE::ActorHandle actor, Last& out) {
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.actor == actor; });
    if (found == entries.end()) return false;
    out = {found->index, clock - found->at, found->completed};
    return true;
}

void ResetOnLoad() {
    entries.clear();
    clock = 0.0f;
}

}  // namespace AnimationInjector::ActionHistory
