#pragma once
#include <cstdint>
#include <string>
#include <string_view>
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
// `qualifier` (the loader passes the set's id, Settings.cpp) is put before every action id with an underscore, the ids
// the file writes and the ones it makes, and before every name of them the file holds: the runtime's actions,
// graph events (AI_<qualifier>_<id>_…) and clips are then the file's own. Empty: the ids as written.
bool Compile(const pugi::xml_document& source, const std::string& sourceName, std::vector<ActionSpec>& output,
    std::vector<EventRuleSpec>& events, std::string& mod, std::string& error, KeyResolver resolveKey,
    std::string_view qualifier = {});
// The same, keeping only the actions (tools and tests that read actions).
bool Compile(const pugi::xml_document& source, const std::string& sourceName, std::vector<ActionSpec>& output,
    std::string& mod, std::string& error, KeyResolver resolveKey, std::string_view qualifier = {});
}
