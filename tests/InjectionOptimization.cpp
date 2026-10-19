#include "PCH.h"
#include "Settings.h"
#include "runtime/DeclarationCatalog.h"
#include "runtime/FlowLinkRoutes.h"
#include "core/NameIndex.h"
#include "core/BoundedNodeSearch.h"
#include <iostream>
#include <cstdlib>
#include <random>
using namespace AnimationInjector;
void Check(bool ok, const char* name) {
    if (!ok) {
        std::cerr << "FAIL: " << name << '\n';
        std::exit(2);
    }
}
// Frozen 0.12.95 implementation: comparison oracle, not used by production.
std::vector<std::string> OldFiles(const std::vector<ActionSpec>& g_specs) {
    std::vector<std::string> out;
    for (const auto& s : g_specs)
        for (const auto& c : s.clips) out.push_back(c.file);
    return out;
}

std::vector<std::string> OldEvents(const std::vector<ActionSpec>& g_specs) {
    std::vector<std::string> out;
    for (const auto& s : g_specs) {
        if (!s.trigger.onEvent.empty()) out.push_back(s.trigger.onEvent);
        if (!s.pairFeatures.cameraReset.empty()) out.push_back(s.pairFeatures.cameraReset);
        for (const auto& link : s.links)
            for (const auto* event : {&link.event, &link.openEvent, &link.closeEvent})
                if (!event->empty()) out.push_back(*event);
        for (const auto& link : s.links) {
            out.push_back(link.enterEvent);
            out.push_back(link.airEnterEvent);
        }
        for (const auto& c : s.clips) out.push_back(c.enterEvent);
        if (!s.paired.enabled && s.condition.inAir != 0)
            for (const auto& c : s.clips) out.push_back(c.airEnterEvent);
        out.push_back(s.events.exit);
        if (s.paired.enabled && s.paired.lossBlend > 0.0f) out.push_back(s.paired.lossExitEvent);
        if (s.recovery.enabled) {
            out.push_back(s.recovery.event);
            out.push_back(s.recovery.recoverEvent);
        }
        for (const SlotWindow* w : {&s.rotate, &s.iframe, &s.hyperArmor})
            if (w->enabled) {
                out.push_back(w->onEvent);
                out.push_back(w->offEvent);
            }
        if (s.cost.enabled) out.push_back(s.cost.event);
        for (const auto& w : s.cancels) {
            out.push_back(w.openEvent);
            out.push_back(w.closeEvent);
            if (!w.vanillaMatches.empty()) out.push_back(w.nativeCancelEvent);
        }
    }
    return out;
}

std::vector<std::string> OldRigFiles(const std::vector<ActionSpec>& specs, PairedRig::Kind rig) {
    std::vector<std::string> out;
    for (const auto& file : OldFiles(specs)) {
        bool wanted = rig == PairedRig::Kind::kCharacter;
        for (const auto& spec : specs)
            if (spec.paired.enabled && PairedRig::SharesAssets(spec.paired.targetRig, rig))
                for (const auto& clip : spec.clips)
                    if (clip.file == file) wanted = true;
        if (wanted) out.push_back(PairedRig::ClipPath(file, rig));
    }
    return out;
}
void CheckCatalog(const std::vector<ActionSpec>& specs) {
    DeclarationCatalog catalog;
    catalog.Build(specs);
    Check(catalog.events == OldEvents(specs), "event order/spelling/duplicates");
    Check(catalog.files == OldFiles(specs), "file order/spelling/duplicates");
    for (const auto& rig : PairedRig::profiles)
        Check(catalog.ForRig(rig.kind) == OldRigFiles(specs, rig.kind), "all 47 rig filters");
    Check(catalog.ForRig(PairedRig::Kind::kUnsupported).empty(), "unsupported rig");
    catalog.Build({});
    Check(catalog.events.empty() && catalog.files.empty(), "catalog rebuild clears old declarations");
    for (const auto& rig : PairedRig::profiles) Check(catalog.ForRig(rig.kind).empty(), "rebuild clears rig list");
}
struct FakeNode;
struct FakeChild {
    FakeNode* node;
};
struct FakeNode {
    std::string name;
    std::vector<FakeChild> children;
};
struct ReadName {
    const char* operator()(FakeNode* node) const { return node->name.c_str(); }
};
struct ReadChildren {
    int operator()(FakeNode* node, std::vector<FakeChild>& out) const {
        if (node->children.size() > 4096) return 0;
        out.assign(node->children.begin(), node->children.end());
        return static_cast<int>(out.size());
    }
};
using Search = BoundedNodeSearch<FakeNode, FakeChild, ReadName, ReadChildren>;
FakeNode* OldSearch(
    FakeNode* node, const char* name, int depth, std::unordered_set<FakeNode*>& seen, std::size_t& allocations) {
    if (!node || depth > 24 || seen.size() >= 8192 || seen.contains(node)) return nullptr;
    seen.insert(node);
    if (_stricmp(node->name.c_str(), name) == 0) return node;
    if (node->children.empty() || node->children.size() > 4096) return nullptr;
    auto buffer = node->children;
    ++allocations;
    for (const auto& child : buffer)
        if (auto* hit = OldSearch(child.node, name, depth + 1, seen, allocations)) return hit;
    return nullptr;
}
int OldFind(const std::vector<std::string>& names, const std::string& value, std::size_t& comparisons) {
    for (std::size_t i = 0; i < names.size(); ++i) {
        ++comparisons;
        if (_stricmp(names[i].c_str(), value.c_str()) == 0) return static_cast<int>(i);
    }
    return -1;
}
template<class F>
double Time(F run) {
    const auto start = std::chrono::steady_clock::now();
    run();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
void NameTests() {
    // Verify equality/hash agreement with CRT, including non-ASCII byte strings.
    for (int i = 1; i < 256; ++i)
        for (int j = 1; j < 256; ++j) {
            const char a[]{static_cast<char>(i), 0}, b[]{static_cast<char>(j), 0};
            const bool equal = _stricmp(a, b) == 0;
            Check(NameEqual{}(a, b) == equal, "CRT equality");
            if (equal) Check(NameHash{}(a) == NameHash{}(b), "equal names share hash");
        }
    std::vector<std::string> table{"first", "SECOND"};
    NameTableLookup adaptive;
    auto find = [&](const char* name) {
        return adaptive.Find(
            table.data(), static_cast<int>(table.size()), [&](int i) { return table[i].c_str(); }, name);
    };
    for (int i = 0; i < 16; ++i) Check(find("second") == 1, "small linear search");
    Check(adaptive.builds == 0, "small lookup does not allocate an index");
    Check(find("first") == 0 && adaptive.builds == 1, "repeated lookup builds index once");
    for (int i = 0; i < 200; ++i) {
        const auto value = "appended_" + std::to_string(i);
        table.push_back(value);
        adaptive.Appended(table.data(), static_cast<int>(table.size()), value.c_str());
    }
    Check(find("APPENDED_199") == 201 && adaptive.builds == 1, "append tracks reallocations without rebuilding");
    table.push_back("external");
    Check(find("external") == 202 && adaptive.builds == 2, "untracked size change rebuilds");
    auto moved = table;
    table.swap(moved);
    Check(find("second") == 1 && adaptive.builds == 3, "untracked table replacement rebuilds");
    table.clear();
    Check(find("first") == -1, "table reset has no stale ID");
    NameIndex index;
    const char* input[]{"Attack", "attack", nullptr, "", "EVENT_01", "Anim_한글"};
    index.Reset(6, [&](int i) { return input[i]; });
    Check(index.Find("ATTACK") == 0 && index.Find("") == 3 && index.Find(nullptr) == -1, "first duplicate/null/empty");
    Check(index.Find("anim_한글") == 5 && index.Find("missing") == -1, "non-ASCII/missing");
    for (int i = 0; i < 20000; ++i) {
        const auto key = "Extra_" + std::to_string(i);
        index.Add(key.c_str(), i + 6);
    }
    Check(index.Find("attack") == 0 && index.Find("extra_19999") == 20005, "append/rehash retains IDs and owned keys");
    index.Reset(1, [](int) { return "new"; });
    Check(index.Find("attack") == -1 && index.Find("new") == 0, "rebuild has no old names");
}
void SearchTests() {
    std::vector<FakeNode> nodes(8500);
    for (int i = 0; i < 8500; ++i) nodes[i].name = "node" + std::to_string(i);
    // A broad hierarchy, duplicate names, cycles and null children.
    for (int i = 1; i < 8500; ++i) nodes[(i - 1) / 4].children.push_back({&nodes[i]});
    nodes[5].name = nodes[10].name = "duplicate";
    nodes[9].children.push_back({&nodes[0]});
    nodes[9].children.push_back({nullptr});
    for (const char* name : {"duplicate", "NODE170", "missing", "node8499"}) {
        Search search;
        std::unordered_set<FakeNode*> seen;
        std::size_t alloc = 0;
        Check(search.Find(&nodes[0], name) == OldSearch(&nodes[0], name, 0, seen, alloc), "DFS identity/cycle/limit");
        Check(search.LastVisited() == seen.size(), "same visited limit");
    }
    std::vector<FakeNode> deep(27);
    for (int i = 0; i < 27; ++i) {
        deep[i].name = std::to_string(i);
        if (i < 26) deep[i].children.push_back({&deep[i + 1]});
    }
    Search search;
    Check(search.Find(&deep[0], "24") == &deep[24] && search.Find(&deep[0], "25") == nullptr, "depth boundary");
    Check(search.FindNative(&deep[0], "0") == &deep[0], "native lookup");
    Check(search.FindNative(&deep[0], "0") == &deep[0] && search.cacheHits == 1, "native hit reuse");
    Check(search.FindNative(&deep[0], "later") == nullptr, "no cached miss");
    FakeNode later{"later", {}};
    deep[0].children.push_back({&later});
    Check(search.FindNative(&deep[0], "later") == &later, "append visible after missed query");
    FakeNode tooWide{"wide", std::vector<FakeChild>(4097, {&later})};
    Check(search.Find(&tooWide, "later") == nullptr, "child bound");
    std::cout << "Search limits, nulls, cycles, duplicate order and append-only cache: PASS\n";
    std::size_t oldAlloc = 0;
    const auto oldMs = Time([&] {
        for (int i = 0; i < 80; ++i) {
            std::unordered_set<FakeNode*> seen;
            Check(OldSearch(&nodes[0], "node170", 0, seen, oldAlloc) != nullptr, "old result");
        }
    });
    Search repeated;
    const auto newMs = Time([&] {
        for (int i = 0; i < 80; ++i) Check(repeated.FindNative(&nodes[0], "node170") != nullptr, "new result");
    });
    std::cout << "BENCH search nodes=8500 queries=80 baselineMs=" << oldMs << " optimizedMs=" << newMs
              << " oldChildAllocations=" << oldAlloc << " newBufferGrowths=" << repeated.bufferGrowths
              << " cacheHits=" << repeated.cacheHits << '\n';
}
void NameBenchmark(std::size_t nativeCount, std::size_t requested) {
    std::vector<std::string> initial, queries;
    for (std::size_t i = 0; i < nativeCount; ++i) initial.push_back("Native_Animation_Event_" + std::to_string(i));
    for (std::size_t i = 0; i < requested; ++i)
        queries.push_back(i % 3 ? "AI_Declared_Event_" + std::to_string(i)
                                : "native_animation_event_" + std::to_string(i % nativeCount));
    std::vector<std::string> oldNames = initial, newNames = initial;
    std::vector<int> oldIDs, newIDs;
    std::size_t comparisons = 0;
    const auto oldMs = Time([&] {
        for (int pass = 0; pass < 2; ++pass)
            for (const auto& q : queries) {
                int id = OldFind(oldNames, q, comparisons);
                if (id < 0) {
                    id = static_cast<int>(oldNames.size());
                    oldNames.push_back(q);
                }
                oldIDs.push_back(id);
            }
    });
    const auto newMs = Time([&] {
        for (int pass = 0; pass < 2; ++pass) {
            NameTableLookup idx;
            for (const auto& q : queries) {
                int id = idx.Find(
                    newNames.data(), static_cast<int>(newNames.size()), [&](int i) { return newNames[i].c_str(); },
                    q.c_str());
                if (id < 0) {
                    id = static_cast<int>(newNames.size());
                    newNames.push_back(q);
                    idx.Appended(newNames.data(), static_cast<int>(newNames.size()), q.c_str());
                }
                newIDs.push_back(id);
            }
        }
    });
    Check(oldNames == newNames && oldIDs == newIDs, "registration results exactly equivalent");
    std::cout << "BENCH names native=" << nativeCount << " requests=" << requested * 2 << " baselineMs=" << oldMs
              << " optimizedMs=" << newMs << " oldComparisons=" << comparisons << '\n';
}
int wmain(int argc, wchar_t** argv) {
    spdlog::set_level(spdlog::level::warn);
    NameTests();
    SearchTests();
    std::vector<ActionSpec> all;
    for (int i = 1; i < argc; ++i) {
        std::vector<ActionSpec> specs;
        Check(Declarations::LoadXmlFile(argv[i], specs), "real XML load");
        ExpandFlowLinkRoutes(specs);
        CheckCatalog(specs);
        all.insert(all.end(), specs.begin(), specs.end());
        std::cout << "Real declaration catalog: actions=" << specs.size() << " PASS\n";
    }
    // Include every creature asset-sharing combination and non-default slots.
    for (const auto& rig : PairedRig::profiles) {
        ActionSpec s;
        s.id = rig.name;
        s.paired.enabled = true;
        s.paired.targetRig = rig.kind;
        s.paired.lossBlend = .6f;
        s.paired.lossExitEvent = "loss";
        ClipVariant c;
        c.file = "Shared.hkx";
        s.clips = {c, c};
        s.trigger.onEvent = "Trigger";
        s.recovery.enabled = true;
        s.rotate.enabled = s.iframe.enabled = s.hyperArmor.enabled = true;
        s.cost.enabled = true;
        s.Finalize();
        all.push_back(s);
    }
    CheckCatalog(all);
    DeclarationCatalog catalog;
    double prep = Time([&] { catalog.Build(all); });
    std::size_t oldCount = 0, newCount = 0;
    auto oldMs = Time([&] {
        for (int i = 0; i < 500; ++i) oldCount += OldEvents(all).size();
    });
    auto newMs = Time([&] {
        for (int i = 0; i < 500; ++i) newCount += catalog.events.size();
    });
    Check(oldCount == newCount, "catalog repeated read");
    std::cout << "BENCH catalog actions=" << all.size() << " reads=500 buildMs=" << prep << " baselineMs=" << oldMs
              << " optimizedReadMs=" << newMs << '\n';
    NameBenchmark(64, 8);
    NameBenchmark(2000, 500);
    NameBenchmark(12000, 4000);
    std::cout << "Injection optimization comparisons: PASS\n";
}
