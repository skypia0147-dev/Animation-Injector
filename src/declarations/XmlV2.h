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
// Validates an AnimationSet document of format 6 (docs/설계-형식6-조건카드.md) and builds its runtime declarations
// directly, with no other format in between: actions Finalized, in document order (then the selecting actions the
// start rules T3-T5 make), each naming `sourceName` as its source, and the file's event registry entries; `mod` is
// the set's id. Only format 6 is read. Atomic: on failure `output` and `events` are left as they were and `error` holds the XML path of the element the author wrote and the reason.
bool Compile(const pugi::xml_document& source, const std::string& sourceName, std::vector<ActionSpec>& output,
    std::vector<EventRuleSpec>& events, std::string& mod, std::string& error, KeyResolver resolveKey);
// The same, keeping only the actions (tools and tests that read actions).
bool Compile(const pugi::xml_document& source, const std::string& sourceName, std::vector<ActionSpec>& output,
    std::string& mod, std::string& error, KeyResolver resolveKey);
}
