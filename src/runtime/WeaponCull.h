#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace RE {
class Actor;
}

namespace AnimationInjector::WeaponCull {
// Weapon and shield nodes hidden while an animation plays: an event function's hide (EventEffects, held
// for its run scope) and a weapon="sheathe" carry (Carry). One ledger
// holds every hide: a node is hidden while any owner holds it, and only its last owner's release
// returns it to the culled state it had before the first. Separate "before" snapshots per system
// restored in the wrong order left a weapon hidden when a carry covered a node a pair had hidden.
using Owner = std::uint64_t;  // 0 = none

// Pure bookkeeping, tested with stand-in nodes (tests/WeaponCull.cpp).
template<class Node>
class Ledger {
public:
    // The first owner of a node records `culledNow` as the state to return to.
    void Hide(Owner owner, const Node& node, bool culledNow) {
        if (!owner) return;
        const auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.node == node; });
        if (it == entries.end())
            entries.push_back({node, culledNow, {owner}});
        else if (std::find(it->owners.begin(), it->owners.end(), owner) == it->owners.end())
            it->owners.push_back(owner);
    }
    // Releases every node `owner` holds; `restore(node, culled)` runs for each node it held last.
    template<class Restore>
    void Show(Owner owner, Restore restore) {
        if (!owner) return;
        for (auto it = entries.begin(); it != entries.end();) {
            std::erase(it->owners, owner);
            if (!it->owners.empty()) {
                ++it;
                continue;
            }
            restore(it->node, it->culled);
            it = entries.erase(it);
        }
    }
    [[nodiscard]] std::size_t Count(Owner owner) const {
        return static_cast<std::size_t>(std::count_if(entries.begin(), entries.end(),
            [&](const Entry& e) { return std::find(e.owners.begin(), e.owners.end(), owner) != e.owners.end(); }));
    }
    [[nodiscard]] bool Empty() const { return entries.empty(); }
    void Clear() { entries.clear(); }

private:
    struct Entry {
        Node node;
        bool culled;
        std::vector<Owner> owners;
    };
    std::vector<Entry> entries;
};

// Main thread only. A new owner token for one hide (a pair role, a held hide, a carry cover).
Owner NewOwner();
// Hides the actor's weapon and shield nodes for `owner`. Called every frame while the hide lasts:
// a weapon equipped since gets a new node, and a node another system showed is culled again.
void Hide(Owner owner, RE::Actor* actor);
// Releases `owner`'s hold; nodes no other owner holds return to their state before the first hide.
void Show(Owner owner);
// Nodes `owner` holds (logs).
std::size_t Count(Owner owner);
// Load / new game: forget every hide without touching the nodes, which belong to the old 3D.
void Reset();
}
