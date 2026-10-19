#pragma once
#include "PCH.h"
#include <algorithm>
#include <cstdint>
#include <vector>

// IFrame immunity: the actor becomes a ghost through the engine's per-actor ghost state (the one Papyrus
// Actor.SetGhost sets): weapons and spells pass through it, it can still hit. Only that actor is immune.
// Up to 0.12.229 the Ghost|Invulnerable flags of its base (TESNPC) were set, so every actor sharing the
// base was immune for the window (a guard's dodge made every guard of that base immune).
//
// Leases are counted per actor: overlapping holders (an action's IFrame window, the iframeOn event
// function) keep it a ghost until the last lets go. An actor that was a ghost before the first lease (a
// ghost NPC, a quest's SetGhost) is never touched.
//
// The game saves the ghost state with the actor. The co-save records the leases held when a save is
// written; after the load each of those actors that was not a ghost before is turned back (PostLoad).
namespace AnimationInjector::ActorImmunity {
struct SavedLease {
    RE::FormID actor = 0;
    bool wasGhost = false;  // the actor's state before its first lease
};
// A 0.12.229 co-save lease: the base whose flags it set, with the bits the base had before.
struct SavedBase {
    RE::FormID base = 0;
    bool ghost = false, invulnerable = false;
};

// Pure bookkeeping, tested without the engine (tests/Lifecycle.cpp).
class Ledger {
public:
    // Adds a lease on `actor`; `wasGhost` is used only by the first. Returns whether to make it a ghost.
    bool Acquire(RE::FormID actor, bool wasGhost) {
        if (!actor) return false;
        if (auto* entry = Find(actor)) {
            ++entry->count;
            return false;
        }
        entries.push_back({actor, wasGhost, 1});
        return !wasGhost;
    }
    // Ends one lease on `actor`. Returns whether to turn it back (its last lease, not a ghost before).
    bool Release(RE::FormID actor) {
        auto* entry = Find(actor);
        if (!entry || --entry->count > 0) return false;
        const bool back = !entry->wasGhost;
        std::erase_if(entries, [&](const Entry& e) { return e.actor == actor; });
        return back;
    }
    [[nodiscard]] std::vector<SavedLease> Snapshot() const {
        std::vector<SavedLease> out;
        for (const auto& e : entries) out.push_back({e.actor, e.wasGhost});
        return out;
    }
    // After a load, with the co-save's leases: returns the actors to turn back. An actor leased again
    // since the load keeps its ghost state; the saved state becomes the one its last release restores.
    std::vector<RE::FormID> Restore(const std::vector<SavedLease>& saved) {
        std::vector<RE::FormID> back;
        for (const auto& s : saved) {
            if (auto* entry = Find(s.actor))
                entry->wasGhost = s.wasGhost;
            else if (s.actor && !s.wasGhost)
                back.push_back(s.actor);
        }
        return back;
    }
    // Session end: returns the actors to turn back, and drops every lease.
    std::vector<RE::FormID> Clear() {
        std::vector<RE::FormID> back;
        for (const auto& e : entries)
            if (!e.wasGhost) back.push_back(e.actor);
        entries.clear();
        return back;
    }
    [[nodiscard]] std::size_t Size() const { return entries.size(); }
    // How many leases `actor` has (0 when none).
    [[nodiscard]] unsigned Holders(RE::FormID actor) const {
        const auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.actor == actor; });
        return it == entries.end() ? 0 : it->count;
    }

private:
    struct Entry {
        RE::FormID actor;
        bool wasGhost;
        unsigned count;
    };
    Entry* Find(RE::FormID actor) {
        const auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.actor == actor; });
        return it == entries.end() ? nullptr : &*it;
    }
    std::vector<Entry> entries;
};

// Main thread only. Returns the lease (the actor's form id), 0 when there is no actor.
RE::FormID Acquire(RE::Actor* actor);
// Ends the lease and clears it; the actor is turned back when no one else holds it.
void Release(RE::FormID& lease);
// The leases held now, for the co-save (runtime/Carry.cpp writes the record).
std::vector<SavedLease> Snapshot();
// The co-save's leases of the save being loaded (form ids already resolved).
void SetLoaded(std::vector<SavedLease> saved);
void SetLoadedBases(std::vector<SavedBase> saved);
// After a load: each actor leased when the save was written is turned back, and each base a 0.12.229
// lease had flagged gets its bits back. Returns how many.
std::size_t PostLoad();
// Load / new game, after every holder released its lease: a lease still held ends now. Returns how
// many were left; the co-save's loaded lists are dropped.
std::size_t ResetOnLoad();
}
