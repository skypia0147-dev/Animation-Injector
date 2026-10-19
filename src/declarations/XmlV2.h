#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <pugixml.hpp>

namespace AnimationInjector {
struct ActionSpec;
struct EventRuleSpec;
}

namespace AnimationInjector::XmlV2 {
// A pure declaration compiler. No actors, graph objects or game callbacks.
using KeyResolver = std::uint32_t (*)(const char*);
// Validates an AnimationSet document (version 2, or 3 with <EventRegistry>) and builds its runtime
// declarations directly: actions Finalized, in document order, each naming `sourceName` as its source,
// and the file's event registry entries; `mod` is the set's id. Atomic: on failure `output` and
// `events` are left as they were and `error` holds the XML path and reason.
bool Compile(const pugi::xml_document& source, const std::string& sourceName, std::vector<ActionSpec>& output,
    std::vector<EventRuleSpec>& events, std::string& mod, std::string& error, KeyResolver resolveKey);
// The same, keeping only the actions (tools and tests that read actions).
bool Compile(const pugi::xml_document& source, const std::string& sourceName, std::vector<ActionSpec>& output,
    std::string& mod, std::string& error, KeyResolver resolveKey);
}
