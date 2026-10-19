#pragma once
#include "runtime/EventRegistrySpec.h"
#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

// Every declaration file's <EventRegistry> merged into one table (decision D3: all functions of an
// event run). Pure, for runtime/EventRegistry.cpp and its tests.
namespace AnimationInjector::EventRegistryMerge {
struct Entry {
    std::string name;                          // the first spelling (names match ignoring case, as graphs do)
    std::vector<EventFunctionSpec> functions;  // every file's, an equal function once
    std::vector<std::string> values;           // every file's payload spellings, once (ignoring case)
    std::vector<std::string> sources;          // the files that register it, in load order
    std::vector<QteSpec> qtes;                 // every file's
};
struct Result {
    std::vector<Entry> entries;      // in first registration order
    std::vector<std::string> notes;  // an event several files give different functions (logged)
    std::size_t merged = 0;          // functions dropped because an equal one was registered
};
inline std::string Key(const std::string& name) {
    std::string key = name;
    for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return key;
}
inline Result Merge(const std::vector<EventRuleSpec>& rules) {
    Result out;
    std::unordered_map<std::string, std::size_t> index;
    for (const auto& rule : rules) {
        const auto [it, inserted] = index.try_emplace(Key(rule.name), out.entries.size());
        if (inserted) out.entries.push_back({rule.name, {}, {}, {}, {}});
        auto& entry = out.entries[it->second];
        const bool another = !entry.sources.empty() &&
                             std::find(entry.sources.begin(), entry.sources.end(), rule.source) == entry.sources.end();
        if (std::find(entry.sources.begin(), entry.sources.end(), rule.source) == entry.sources.end())
            entry.sources.push_back(rule.source);
        for (const auto& value : rule.values)
            if (std::none_of(entry.values.begin(), entry.values.end(),
                    [&](const std::string& v) { return Key(v) == Key(value); }))
                entry.values.push_back(value);
        entry.qtes.insert(entry.qtes.end(), rule.qtes.begin(), rule.qtes.end());
        bool added = false;
        for (const auto& function : rule.functions) {
            if (std::find(entry.functions.begin(), entry.functions.end(), function) != entry.functions.end()) {
                ++out.merged;
                continue;
            }
            entry.functions.push_back(function);
            added = true;
        }
        if (another && added)
            out.notes.push_back(
                "'" + entry.name + "': " + rule.source + " adds functions to those of " + entry.sources.front());
    }
    return out;
}
}
