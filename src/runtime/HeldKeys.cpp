#include "runtime/HeldKeys.h"

namespace AnimationInjector::HeldKeys {
Tracker::Record* Tracker::Find(Key key) {
    for (auto& r : _records)
        if (r.key == key) return &r;
    return nullptr;
}
const Tracker::Record* Tracker::Find(Key key) const {
    for (const auto& r : _records)
        if (r.key == key) return &r;
    return nullptr;
}

void Tracker::Press(Key key, float) {
    auto* r = Find(key);
    if (!r) r = &_records.emplace_back(Record{key});
    r->down = true;
}

void Tracker::Release(Key key, float now) {
    if (auto* r = Find(key); r && r->down) {
        r->down = false;
        r->upAt = now;
    }
}

bool Tracker::Held(Key key) const {
    const auto* r = Find(key);
    return r && r->down;
}

void Tracker::SetChain(Key key, float downAt) {
    _chain = key;
    _chainDownAt = downAt;
}

float Tracker::ChainHeldTime(float now) const {
    if (!_chain) return -1.0f;
    const auto* r = Find(*_chain);
    // Up since it started the action: until its release; still down (or never seen go up): until now.
    const float until = r && !r->down && r->upAt >= _chainDownAt ? r->upAt : now;
    return until >= _chainDownAt ? until - _chainDownAt : 0.0f;
}

void Tracker::Reset() {
    _moveDirection = -1;
    _records.clear();
    _chain.reset();
    _chainDownAt = 0.0f;
}

Tracker& Get() {
    static Tracker tracker;
    return tracker;
}
}  // namespace AnimationInjector::HeldKeys
