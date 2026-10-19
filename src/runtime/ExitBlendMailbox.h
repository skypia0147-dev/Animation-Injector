#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstddef>

namespace AnimationInjector {
// The caller synchronizes this bounded mailbox. Identities are never dereferenced.
// Tokens belong to activation, not to a later action using the same graph.
template<class Token, std::size_t Capacity = 64>
class ExitBlendMailbox {
public:
    struct Key {
        std::uintptr_t node = 0, character = 0;
        bool operator==(const Key&) const = default;
    };
    bool Activate(Key key, const Token& token) {
        for (auto& item : items)
            if (item.active && item.key == key) {
                item = {key, token, true, false};
                return true;
            }
        for (auto& item : items)
            if (!item.active) {
                item = {key, token, true, false};
                return true;
            }
        return false;
    }
    float Observe(Key key, float delta, bool held) {
        if (!held || !std::isfinite(delta) || delta <= 0) return delta;
        for (auto& item : items)
            if (item.active && item.key == key) item.pending = true;
        return delta;
    }
    void Deactivate(Key key) {
        for (auto& item : items)
            if (item.active && item.key == key) item = {};
    }
    std::size_t Take(std::array<Token, Capacity>& result) {
        std::size_t count = 0;
        for (auto& item : items)
            if (item.active && item.pending) {
                result[count++] = item.token;
                item.pending = false;
            }
        return count;
    }
    void Reset() { items = {}; }
    // Items held now: none means no key can be deactivated (the hooks skip the lock then).
    [[nodiscard]] std::size_t Active() const {
        std::size_t count = 0;
        for (const auto& item : items) count += item.active ? 1 : 0;
        return count;
    }

private:
    struct Item {
        Key key;
        Token token;
        bool active = false, pending = false;
    };
    std::array<Item, Capacity> items{};
};
}
