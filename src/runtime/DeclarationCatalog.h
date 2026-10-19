#pragma once
#include "runtime/ActionSpec.h"
#include "core/NameIndex.h"
#include "core/FullBodyEntryNames.h"
#include "core/PairEntryNames.h"
#include <unordered_set>

namespace AnimationInjector {
// Immutable between declaration loads. Preserve order, spelling, duplicates and
// generated flow-route events exactly; graph-local IDs remain engine-owned.
struct DeclarationCatalog {
    std::vector<std::string> files, events;
    // The files of the actions that play in third person (view third or both, and every pair: the
    // character's third-person projects) and in first person (non-paired, view first or both: the
    // first-person project), in the same order (runtime/ViewSpec.h).
    std::vector<std::string> thirdPersonFiles, firstPersonFiles;
    // events without repeats, ignoring case (first spelling, first-seen order). Graph event tables
    // and project maps match names ignoring case, so registering this list gives the same ids as
    // the full one; with thousands of actions the full list repeats most names many times.
    std::vector<std::string> uniqueEvents;
    // The registry's names alone, without repeats: they go into every graph of a project that can
    // receive an action (core/GraphInjector.cpp), since any clip there may carry them as annotations.
    std::vector<std::string> registryEvents;
    std::array<std::vector<std::string>, PairedRig::profiles.size() + 1> rigFiles;
    // `registered`: the names of the global event registry (runtime/EventRegistry.h), registered in the
    // graphs like the actions' own events so that clip annotations with these names are sent.
    void Build(const std::vector<ActionSpec>& specs, const std::vector<std::string>& registered = {}) {
        files.clear();
        thirdPersonFiles.clear();
        firstPersonFiles.clear();
        events.clear();
        uniqueEvents.clear();
        registryEvents.clear();
        for (auto& list : rigFiles) list.clear();
        for (const auto& s : specs)
            for (const auto& c : s.clips) {
                files.push_back(c.file);
                if (s.paired.enabled || InThird(s.view.view)) thirdPersonFiles.push_back(c.file);
                if (!s.paired.enabled && InFirst(s.view.view)) firstPersonFiles.push_back(c.file);
            }
        for (const auto& name : registered) events.push_back(name);
        for (const auto& s : specs) {
            if (!s.trigger.onEvent.empty()) events.push_back(s.trigger.onEvent);
            if (!s.pairFeatures.cameraReset.empty()) events.push_back(s.pairFeatures.cameraReset);
            for (const auto& link : s.links)
                for (const auto* event : {&link.event, &link.openEvent, &link.closeEvent})
                    if (!event->empty()) events.push_back(*event);
            for (const auto& link : s.links) {
                events.push_back(link.enterEvent);
                events.push_back(link.airEnterEvent);
            }
            for (const auto& c : s.clips) events.push_back(c.enterEvent);
            if (!s.paired.enabled && s.condition.inAir != 0)
                for (const auto& c : s.clips) events.push_back(c.airEnterEvent);
            events.push_back(s.events.exit);
            if (s.paired.enabled && s.paired.lossBlend > 0.0f) events.push_back(s.paired.lossExitEvent);
            if (s.recovery.enabled) {
                events.push_back(s.recovery.event);
                events.push_back(s.recovery.recoverEvent);
            }
            for (const SlotWindow* w : {&s.rotate, &s.iframe, &s.hyperArmor})
                if (w->enabled) {
                    events.push_back(w->onEvent);
                    events.push_back(w->offEvent);
                }
            for (const auto& q : s.qtes) {
                if (!q.event.empty()) events.push_back(q.event);
                if (!q.stopEvent.empty()) events.push_back(q.stopEvent);
                for (const auto& r : q.results)
                    if (!r.event.empty()) events.push_back(r.event);
            }
            if (s.cost.enabled) events.push_back(s.cost.event);
            if (s.carryRelease.enabled && !s.carryRelease.event.empty()) events.push_back(s.carryRelease.event);
            for (const auto& w : s.cancels) {
                events.push_back(w.openEvent);
                events.push_back(w.closeEvent);
                if (!w.vanillaMatches.empty()) events.push_back(w.nativeCancelEvent);
            }
        }

        std::unordered_set<std::string_view, NameHash, NameEqual> seen;
        for (const auto& event : events)
            if (seen.insert(event).second) uniqueEvents.push_back(event);
        // `registered` leads `events`, so its first spellings are the ones kept above.
        std::unordered_set<std::string_view, NameHash, NameEqual> seenRegistered;
        for (const auto& name : registered)
            if (seenRegistered.insert(name).second) registryEvents.push_back(name);
        // The shared full-body entries every full-body action enters through (core/FullBodyEntry.h).
        if (std::any_of(specs.begin(), specs.end(), [](const auto& s) { return !s.paired.enabled; }))
            for (const char* shared : {FullBodyEntry::kEnterEvent, FullBodyEntry::kLinkEnterEvent})
                if (seen.insert(shared).second) uniqueEvents.push_back(shared);
        // The shared pair entries every paired action enters through (core/PairEntry.h).
        if (std::any_of(specs.begin(), specs.end(), [](const auto& s) { return s.paired.enabled; }))
            for (const char* shared : {PairEntry::kEnterLeadEvent, PairEntry::kEnterSupportEvent})
                if (seen.insert(shared).second) uniqueEvents.push_back(shared);

        // Exact file membership matches the old creature filtering loop. Iterate
        // the global list afterwards to retain cross-project binding order.
        for (const auto& rig : PairedRig::profiles) {
            if (rig.kind == PairedRig::Kind::kCharacter) continue;
            std::unordered_set<std::string> wanted;
            for (const auto& s : specs)
                if (s.paired.enabled && PairedRig::SharesAssets(s.paired.targetRig, rig.kind))
                    for (const auto& c : s.clips) wanted.insert(c.file);
            auto& list = rigFiles[static_cast<std::size_t>(rig.kind)];
            for (const auto& file : files)
                if (wanted.contains(file)) list.push_back(PairedRig::ClipPath(file, rig.kind));
        }
    }
    const std::vector<std::string>& ForRig(PairedRig::Kind rig) const {
        if (rig == PairedRig::Kind::kCharacter) return thirdPersonFiles;
        const auto i = static_cast<std::size_t>(rig);
        return rigFiles[i < rigFiles.size() ? i : 0];
    }
};
}
