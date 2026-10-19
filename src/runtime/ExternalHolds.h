#pragma once
#include <algorithm>
#include <vector>

namespace AnimationInjector {
// Our entries in another plugin's per-actor opt-out list (runtime/PrecisionLink). The toggle reports whether
// that list changed: an actor someone else had already opted out is not ours, so it is never given back.
// Release gives back only what Hold added; ReleaseAll at load (the other plugin keeps its list across loads).
template<class Key>
class ExternalHolds {
public:
    // toggle(key, true) disables and returns true when this call added the actor.
    template<class Toggle>
    bool Hold(const Key& key, Toggle&& toggle) {
        if (Holds(key)) return true;
        if (!toggle(key, true)) return false;
        held_.push_back(key);
        return true;
    }
    template<class Toggle>
    bool Release(const Key& key, Toggle&& toggle) {
        const auto it = std::find(held_.begin(), held_.end(), key);
        if (it == held_.end()) return false;
        held_.erase(it);
        toggle(key, false);
        return true;
    }
    template<class Toggle>
    void ReleaseAll(Toggle&& toggle) {
        const auto held = std::move(held_);
        held_.clear();
        for (const auto& key : held) toggle(key, false);
    }
    bool Holds(const Key& key) const { return std::find(held_.begin(), held_.end(), key) != held_.end(); }
    std::size_t Count() const { return held_.size(); }

private:
    std::vector<Key> held_;
};
}
