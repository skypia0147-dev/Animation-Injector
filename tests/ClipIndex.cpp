// Stage 3 (R-6): the clip index answers exactly like the former linear FindClip, and costs less.
// usage: AnimationInjectorClipIndexTests <vanilla-clip-names.txt> <declaration.xml>...
#include "Settings.h"
#include "runtime/ClipIndex.h"
#include <chrono>
#include <fstream>
#include <iostream>

using namespace AnimationInjector;
namespace {
// The former Actions::FindClip body, over the same declarations.
bool LinearFind(const std::vector<ActionSpec>& specs, const char* name, int& action, int& variant) {
    if (!name) return false;
    for (int a = 0; a < static_cast<int>(specs.size()); ++a) {
        const int v = specs[a].VariantByClipName(name);
        if (v >= 0) {
            action = a;
            variant = v;
            return true;
        }
    }
    return false;
}
int failures = 0;
void Check(bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}
}

int wmain(int argc, wchar_t** argv) {
    spdlog::set_level(spdlog::level::err);
    if (argc < 3) {
        std::cerr << "usage: AnimationInjectorClipIndexTests <vanilla-names.txt> <declaration.xml>...\n";
        return 1;
    }
    std::vector<std::string> vanilla;
    {
        std::ifstream in(argv[1]);
        for (std::string line; std::getline(in, line);)
            if (!line.empty()) vanilla.push_back(line);
    }
    std::vector<ActionSpec> specs;
    for (int i = 2; i < argc; ++i) Declarations::LoadXmlFile(argv[i], specs);
    ClipIndex index;
    index.Build(specs);

    std::vector<std::string> ours;
    for (const auto& s : specs)
        for (const auto& c : s.clips) ours.push_back(c.clipName);
    std::vector<std::string> probes = vanilla;
    for (const auto& name : ours) {
        probes.push_back(name);
        std::string upper = name, lower = name;
        for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        probes.push_back(upper);
        probes.push_back(lower);
        probes.push_back(name + "X");
        probes.push_back(name.substr(0, name.size() - 1));
    }
    for (const char* edge : {"", "A", "AI", "AI_", "ai_", "Ai_Missing_ClipF", "AI_Clip", "Al_Dodge_ClipF"})
        probes.emplace_back(edge);

    // Equivalence: found/not found, action and variant for every probe.
    for (const auto& name : probes) {
        int a1 = -1, v1 = -1, a2 = -1, v2 = -1;
        const bool linear = LinearFind(specs, name.c_str(), a1, v1), indexed = index.Find(name.c_str(), a2, v2);
        Check(linear == indexed && a1 == a2 && v1 == v2, "'" + name + "'");
    }
    int a = -1, v = -1;
    Check(!index.Find(nullptr, a, v), "null name");
    Check(ours.empty() || index.Size() > 0, "index built");

    // Cost per lookup, vanilla names (every non-injected clip) and our own names.
    const auto measure = [&](const std::vector<std::string>& names, auto&& find) {
        const int rounds = 200;
        std::size_t hits = 0;
        const auto start = std::chrono::steady_clock::now();
        for (int r = 0; r < rounds; ++r)
            for (const auto& n : names) {
                int x = -1, y = -1;
                hits += find(n.c_str(), x, y);
            }
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
        return std::pair{ns / (static_cast<double>(rounds) * names.size()), hits};
    };
    const auto linear = [&](const char* n, int& x, int& y) { return LinearFind(specs, n, x, y); };
    const auto indexed = [&](const char* n, int& x, int& y) { return index.Find(n, x, y); };
    const auto [lv, lvHits] = measure(vanilla, linear);
    const auto [iv, ivHits] = measure(vanilla, indexed);
    const auto [lo, loHits] = measure(ours, linear);
    const auto [io, ioHits] = measure(ours, indexed);
    Check(lvHits == ivHits && loHits == ioHits, "benchmark hit counts");
    std::cout << "ClipIndex: actions=" << specs.size() << " clips=" << ours.size() << " probes=" << probes.size()
              << " | vanilla ns/lookup linear=" << lv << " index=" << iv << " | ours ns/lookup linear=" << lo
              << " index=" << io << '\n';
    if (failures) {
        std::cerr << failures << " failures\n";
        return 1;
    }
    std::cout << "ClipIndex: identical to the linear lookup for every probe\n";
    return 0;
}
