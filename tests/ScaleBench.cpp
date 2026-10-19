// Scale bench: the plugin's declaration pipeline at load (Actions::Load) over a folder laid out like
// Data/SKSE/Plugins/AnimationInjector, e.g. one made by tools/scale/generate_stress.py.
//   AnimationInjectorScaleBench <root> [repeat] [nolog]
// Logs go to a file sink flushed on info, as in the game, so per-file log cost is included.
#include "Settings.h"
#include "runtime/ClipIndex.h"
#include "runtime/DeclarationCatalog.h"
#include "runtime/EventRegistryMerge.h"
#include "runtime/FlowLinkRoutes.h"
#include "core/NameIndex.h"
#include <unordered_set>
#include "declarations/XmlV2.h"
#include <pugixml.hpp>
#include <chrono>
#include <map>
#include <iostream>
#include <spdlog/sinks/basic_file_sink.h>

using namespace AnimationInjector;
namespace {
using Clock = std::chrono::steady_clock;
double Ms(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: AnimationInjectorScaleBench <root> [repeat]\n";
        return 2;
    }
    const int repeat = argc > 2 ? std::max(1, std::atoi(argv[2])) : 1;
    auto log = std::filesystem::temp_directory_path() / "AnimationInjectorScaleBench.log";
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log.string(), true);
    spdlog::set_default_logger(std::make_shared<spdlog::logger>("log", sink));
    // Optional third argument "nolog": the same run without log output, to separate its cost.
    spdlog::set_level(argc > 3 && std::string_view(argv[3]) == "nolog" ? spdlog::level::off : spdlog::level::info);
    spdlog::flush_on(spdlog::level::info);
    {
        // Split of the per-file cost: reading + XML parse vs. declaration compile.
        std::vector<std::filesystem::path> xmls;
        for (const auto& dir : std::filesystem::directory_iterator(argv[1]))
            for (const auto& f : std::filesystem::directory_iterator(dir.path()))
                if (f.path().extension() == ".xml") xmls.push_back(f.path());
        double parse = 0, compile = 0;
        for (const auto& f : xmls) {
            auto start = Clock::now();
            pugi::xml_document doc;
            doc.load_file(f.c_str());
            parse += Ms(start);
            start = Clock::now();
            std::vector<ActionSpec> out;
            std::vector<EventRuleSpec> events;
            std::string mod, error;
            XmlV2::Compile(doc, f.string(), out, events, mod, error, Declarations::ResolveKeyName);
            compile += Ms(start);
        }
        std::cout << "split files=" << xmls.size() << " parseMs=" << parse << " compileMs=" << compile << "\n";
    }
    for (int r = 0; r < repeat; ++r) {
        auto start = Clock::now();
        std::vector<EventRuleSpec> events;
        auto specs = Declarations::LoadAllFrom(argv[1], &events);
        const double load = Ms(start);
        // The global event registry (runtime/EventRegistry::Build): every file's rules merged.
        start = Clock::now();
        const auto merged = EventRegistryMerge::Merge(events);
        const double mergeMs = Ms(start);
        std::vector<std::string> registered;
        registered.reserve(events.size());
        for (const auto& rule : events) registered.push_back(rule.name);
        start = Clock::now();
        ExpandFlowLinkRoutes(specs);
        const double routes = Ms(start);
        start = Clock::now();
        ClipIndex clips;
        clips.Build(specs);
        const double index = Ms(start);
        start = Clock::now();
        DeclarationCatalog catalog;
        catalog.Build(specs, registered);
        const double catalogMs = Ms(start);
        std::size_t links = 0, clipCount = 0;
        for (const auto& s : specs) {
            links += s.links.size();
            clipCount += s.clips.size();
        }
        if (r == 0) {
            // Third-person RootBehaviorGraph wildcard entries by kind, as GraphInjector adds them:
            // full-body (entry clips, incoming links of single-clip destinations), air route
            // (inAir actions: entry clips and incoming links), paired (two roles).
            std::size_t fullBody = 0, fullBodyLinks = 0, air = 0, airLinks = 0, paired = 0;
            std::unordered_map<std::string, int> ids;
            for (int i = 0; i < static_cast<int>(specs.size()); ++i) ids.try_emplace(specs[i].id, i);
            std::vector<std::size_t> incoming(specs.size(), 0);
            for (const auto& s : specs)
                if (!s.paired.enabled && s.clips.size() == 1)
                    for (const auto& l : s.links)
                        if (const auto it = ids.find(l.action); it != ids.end() && specs[it->second].id != s.id)
                            ++incoming[it->second];
            for (std::size_t i = 0; i < specs.size(); ++i) {
                const auto& s = specs[i];
                if (s.paired.enabled) {
                    paired += 2;
                    continue;
                }
                std::size_t entries = 0;
                for (const auto& c : s.clips) entries += c.entry ? 1 : 0;
                fullBody += entries;
                if (s.clips.size() == 1) fullBodyLinks += incoming[i];
                if (s.condition.inAir != 0) {
                    air += entries;
                    if (s.clips.size() == 1) airLinks += incoming[i];
                }
            }
            // Since 0.12.209 full-body entries share two root wildcards (core/FullBodyEntry.h), and
            // paired entries two more (core/PairEntry.h); air routes keep their own.
            const std::size_t shared = (fullBody + fullBodyLinks ? 2 : 0) + (paired ? 2 : 0);
            // Actions per start key: a press walks them all (InputSink).
            std::map<std::uint32_t, std::size_t> perKey;
            for (const auto& s : specs)
                if (s.trigger.key) ++perKey[s.trigger.key];
            std::cout << "start keys:";
            for (const auto& [key, count] : perKey) std::cout << " " << key << "=" << count;
            std::cout << "\n";
            std::cout << "root wildcards per action: fullBody=" << fullBody << " fullBodyLinks=" << fullBodyLinks
                      << " air=" << air << " airLinks=" << airLinks << " paired=" << paired
                      << " total=" << (fullBody + fullBodyLinks + air + airLinks + paired)
                      << " | with shared entries: " << (air + airLinks + shared) << "\n";
            // Event registration into a project map (core/EventMaps.cpp) on plain containers: the
            // 0.12.207 scans of both tables per name, on a sample then scaled, against 0.12.208's
            // indexes over every name. The master table is the distinct names after 1,800 native ones.
            std::vector<std::string> master;
            for (int i = 0; i < 1800; ++i) master.push_back("native" + std::to_string(i));
            master.insert(master.end(), catalog.uniqueEvents.begin(), catalog.uniqueEvents.end());
            // The old cost per name grows with the map: time a sample from the middle of the list with
            // the map holding half of the names, the average state over the whole registration.
            std::vector<std::string> keys(master.begin(), master.begin() + 1800 + catalog.uniqueEvents.size() / 2);
            const std::size_t sample = std::min<std::size_t>(2000, catalog.events.size());
            const std::size_t from = (catalog.events.size() - sample) / 2;
            start = Clock::now();
            for (std::size_t n = from; n < from + sample; ++n) {
                const auto& name = catalog.events[n];
                int id = -1;
                for (std::size_t i = 0; i < master.size() && id < 0; ++i)
                    if (_stricmp(master[i].c_str(), name.c_str()) == 0) id = static_cast<int>(i);
                bool have = false;
                for (const auto& k : keys)
                    if (_stricmp(k.c_str(), name.c_str()) == 0) {
                        have = true;
                        break;
                    }
                if (id >= 0 && !have) keys.push_back(name);
            }
            const double oldSample = Ms(start);
            start = Clock::now();
            NameIndex masterNames;
            masterNames.Reset(static_cast<int>(master.size()), [&](int i) { return master[i].c_str(); });
            std::unordered_set<std::string, NameHash, NameEqual> present(master.begin(), master.begin() + 1800);
            std::size_t added = 0;
            for (const auto& name : catalog.uniqueEvents)
                if (masterNames.Find(name.c_str()) >= 0 && present.emplace(name).second) ++added;
            const double indexed = Ms(start);
            std::cout << "eventmap names=" << catalog.events.size() << " distinct=" << catalog.uniqueEvents.size()
                      << " | old: " << sample << " names " << oldSample << " ms -> all names ~"
                      << oldSample * static_cast<double>(catalog.events.size()) / static_cast<double>(sample) / 1000.0
                      << " s per project | new: all distinct names " << indexed << " ms (added " << added << ")\n";
        }
        std::cout << "actions=" << specs.size() << " clips=" << clipCount << " links=" << links
                  << " files=" << catalog.files.size() << " events=" << catalog.events.size()
                  << " distinct=" << catalog.uniqueEvents.size() << " | loadMs=" << load << " routesMs=" << routes
                  << " clipIndexMs=" << index << " catalogMs=" << catalogMs
                  << " totalMs=" << (load + routes + index + catalogMs + mergeMs) << "\n";
        std::size_t functions = 0, withFunctions = 0;
        for (const auto& e : merged.entries) {
            functions += e.functions.size();
            withFunctions += e.functions.empty() ? 0 : 1;
        }
        std::cout << "registry rules=" << events.size() << " events=" << merged.entries.size()
                  << " withFunctions=" << withFunctions << " functions=" << functions << " merged=" << merged.merged
                  << " notes=" << merged.notes.size() << " mergeMs=" << mergeMs << "\n";
    }
    return 0;
}
